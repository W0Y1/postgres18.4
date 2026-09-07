#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "portability/instr_time.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"

#include "online_prediction.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

PG_FUNCTION_INFO_V1(plan_replay_last_timing);
PG_FUNCTION_INFO_V1(plan_replay_reset_stats);

typedef enum PlanReplayMode
{
	PLAN_REPLAY_MODE_OFF,
	PLAN_REPLAY_MODE_CAPTURE,
	PLAN_REPLAY_MODE_REPLAY
} PlanReplayMode;

typedef enum PlanReplayChannelMode
{
	PLAN_REPLAY_CHANNEL_INHERIT,
	PLAN_REPLAY_CHANNEL_OFF,
	PLAN_REPLAY_CHANNEL_CAPTURE,
	PLAN_REPLAY_CHANNEL_REPLAY,
	PLAN_REPLAY_CHANNEL_CAPTURE_REPLAY,
	PLAN_REPLAY_CHANNEL_PREDICT,
	PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT
} PlanReplayChannelMode;

#define PLAN_REPLAY_PATH_ID_BYTES 16
#define PLAN_REPLAY_PATH_ID_HEX_LENGTH (PLAN_REPLAY_PATH_ID_BYTES * 2)
#define PLAN_REPLAY_COST_LINE_MAX 256

typedef struct PlanReplayPathId
{
	uint64		hi;
	uint64		lo;
} PlanReplayPathId;

typedef struct PlanReplayIdentityEntry
{
	PlanReplayPathId id;
	char	   *descriptor;
} PlanReplayIdentityEntry;

typedef struct PlanReplayCostEntry
{
	PlanReplayPathId id;
	Cost		startup_cost;
	Cost		total_cost;
	bool		matched;
} PlanReplayCostEntry;

static int plan_replay_mode = PLAN_REPLAY_MODE_OFF;
static int plan_replay_path_mode = PLAN_REPLAY_CHANNEL_INHERIT;
static int plan_replay_cardinality_mode = PLAN_REPLAY_CHANNEL_INHERIT;
static char *plan_replay_path_capture_file = NULL;
static char *plan_replay_path_replay_file = NULL;
static char *plan_replay_cardinality_capture_file = NULL;
static char *plan_replay_cardinality_replay_file = NULL;

static planner_hook_type previous_planner_hook = NULL;
static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static cardinality_estimate_hook_type previous_cardinality_estimate_hook = NULL;

static FILE *plan_replay_path_capture_stream = NULL;
static FILE *plan_replay_path_replay_stream = NULL;
static FILE *plan_replay_cardinality_capture_stream = NULL;
static FILE *plan_replay_cardinality_replay_stream = NULL;
static PlannerInfo *plan_replay_planner_root = NULL;
static uint64 plan_replay_path_sequence = 0;
static uint64 plan_replay_batch_count = 0;
static uint64 plan_replay_last_path_count = 0;
static uint64 plan_replay_last_batch_count = 0;
static uint64 plan_replay_cardinality_sequence = 0;
static uint64 plan_replay_cost_replay_entry_count = 0;
static uint64 plan_replay_cost_replay_applied_count = 0;
static uint64 plan_replay_cost_replay_matched_entry_count = 0;
static uint64 plan_replay_last_cost_replay_entry_count = 0;
static uint64 plan_replay_last_cost_replay_applied_count = 0;
static uint64 plan_replay_last_cost_replay_unmatched_count = 0;
static double plan_replay_last_planning_ms = 0;
static PlanReplayOnlineStats plan_replay_current_online_stats;
static PlanReplayOnlineStats plan_replay_last_online_stats;
static int plan_replay_planner_depth = 0;
static bool plan_replay_statement_active = false;
static HTAB *plan_replay_identity_table = NULL;
static HTAB *plan_replay_cost_table = NULL;


static const struct config_enum_entry plan_replay_mode_options[] = {
	{"off", PLAN_REPLAY_MODE_OFF, false},
	{"capture", PLAN_REPLAY_MODE_CAPTURE, false},
	{"replay", PLAN_REPLAY_MODE_REPLAY, false},
	{NULL, 0, false}
};

static const struct config_enum_entry plan_replay_channel_mode_options[] = {
	{"inherit", PLAN_REPLAY_CHANNEL_INHERIT, false},
	{"off", PLAN_REPLAY_CHANNEL_OFF, false},
	{"capture", PLAN_REPLAY_CHANNEL_CAPTURE, false},
	{"replay", PLAN_REPLAY_CHANNEL_REPLAY, false},
	{"capture_replay", PLAN_REPLAY_CHANNEL_CAPTURE_REPLAY, false},
	{"predict", PLAN_REPLAY_CHANNEL_PREDICT, false},
	{"capture_predict", PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT, false},
	{NULL, 0, false}
};

static const struct config_enum_entry plan_replay_cardinality_mode_options[] = {
	{"inherit", PLAN_REPLAY_CHANNEL_INHERIT, false},
	{"off", PLAN_REPLAY_CHANNEL_OFF, false},
	{"capture", PLAN_REPLAY_CHANNEL_CAPTURE, false},
	{"replay", PLAN_REPLAY_CHANNEL_REPLAY, false},
	{"capture_replay", PLAN_REPLAY_CHANNEL_CAPTURE_REPLAY, false},
	{NULL, 0, false}
};

static const char *plan_replay_operator_name(Path *path);
static const char *plan_replay_path_node_name(Path *path);
static const char *plan_replay_join_type_name(JoinType jointype);
static void plan_replay_append_path_json(StringInfo output, Path *path);
static void plan_replay_append_compact_path_json(StringInfo output, Path *path);
static PlanReplayPathId plan_replay_compute_path_id(Path *path);
static void plan_replay_format_path_id(PlanReplayPathId id, char *output);
static void plan_replay_load_costs(void);
static int plan_replay_effective_channel_mode(int channel_mode);
static bool plan_replay_mode_captures(int mode);
static bool plan_replay_mode_replays(int mode);
static bool plan_replay_mode_predicts(int mode);
static bool plan_replay_is_active(void);
static void plan_replay_process_paths(RelOptInfo *rel, List *paths, bool partial);
static Cardinality plan_replay_cardinality_estimate(PlannerInfo *root,
														CardinalityEstimateKind kind,
														RelOptInfo *rel,
														RelOptInfo *outer_rel,
														RelOptInfo *inner_rel,
														SpecialJoinInfo *sjinfo,
															List *restrictlist,
															Cardinality outer_rows,
															Cardinality inner_rows,
															Cardinality native_rows,
															bool *sequence_valid,
																	uint64 *sequence);

static int
plan_replay_effective_channel_mode(int channel_mode)
{
	if (channel_mode != PLAN_REPLAY_CHANNEL_INHERIT)
		return channel_mode;

	switch (plan_replay_mode)
	{
		case PLAN_REPLAY_MODE_OFF:
			return PLAN_REPLAY_CHANNEL_OFF;
		case PLAN_REPLAY_MODE_CAPTURE:
			return PLAN_REPLAY_CHANNEL_CAPTURE;
		case PLAN_REPLAY_MODE_REPLAY:
			return PLAN_REPLAY_CHANNEL_REPLAY;
	}
	return PLAN_REPLAY_CHANNEL_OFF;
}

static bool
plan_replay_mode_captures(int mode)
{
	return mode == PLAN_REPLAY_CHANNEL_CAPTURE ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_REPLAY ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT;
}

static bool
plan_replay_mode_replays(int mode)
{
	return mode == PLAN_REPLAY_CHANNEL_REPLAY ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_REPLAY;
}

static bool
plan_replay_mode_predicts(int mode)
{
	return mode == PLAN_REPLAY_CHANNEL_PREDICT ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT;
}

static const char *
plan_replay_operator_name(Path *path)
{
	if (path->parallel_aware)
	{
		switch (path->pathtype)
		{
			case T_SeqScan: return "Parallel Seq Scan";
			case T_IndexScan: return "Parallel Index Scan";
			case T_IndexOnlyScan: return "Parallel Index Only Scan";
			case T_BitmapHeapScan: return "Parallel Bitmap Heap Scan";
			default: break;
		}
	}

	if (IsA(path, AggPath))
	{
		AggSplit split = ((AggPath *) path)->aggsplit;

		if (DO_AGGSPLIT_SKIPFINAL(split))
			return "Partial Aggregate";
		if (DO_AGGSPLIT_COMBINE(split))
			return "Finalize Aggregate";
	}

	switch (path->pathtype)
	{
		case T_SeqScan: return "Seq Scan";
		case T_SampleScan: return "Sample Scan";
		case T_IndexScan: return "Index Scan";
		case T_IndexOnlyScan: return "Index Only Scan";
		case T_BitmapIndexScan: return "Bitmap Index Scan";
		case T_BitmapHeapScan: return "Bitmap Heap Scan";
		case T_TidScan: return "Tid Scan";
		case T_TidRangeScan: return "Tid Range Scan";
		case T_SubqueryScan: return "Subquery Scan";
		case T_FunctionScan: return "Function Scan";
		case T_TableFuncScan: return "Table Function Scan";
		case T_ValuesScan: return "Values Scan";
		case T_CteScan: return "CTE Scan";
		case T_WorkTableScan: return "WorkTable Scan";
		case T_ForeignScan: return "Foreign Scan";
		case T_CustomScan: return "Custom Scan";
		case T_NestLoop: return "Nested Loop";
		case T_MergeJoin: return "Merge Join";
		case T_HashJoin: return "Hash Join";
		case T_Append: return "Append";
		case T_MergeAppend: return "Merge Append";
		case T_Material: return "Materialize";
		case T_Memoize: return "Memoize";
		case T_Sort: return "Sort";
		case T_IncrementalSort: return "Incremental Sort";
		case T_Group: return "Group";
		case T_Agg: return "Aggregate";
		case T_WindowAgg: return "WindowAgg";
		case T_Unique: return "Unique";
		case T_Gather: return "Gather";
		case T_GatherMerge: return "Gather Merge";
		case T_SetOp: return "SetOp";
		case T_RecursiveUnion: return "Recursive Union";
		case T_LockRows: return "LockRows";
		case T_ModifyTable: return "ModifyTable";
		case T_Limit: return "Limit";
		case T_Result: return "Result";
		case T_ProjectSet: return "ProjectSet";
		default: return "Unknown";
	}
}

static const char *
plan_replay_path_node_name(Path *path)
{
	switch (nodeTag(path))
	{
		case T_Path: return "Path";
		case T_IndexPath: return "IndexPath";
		case T_BitmapHeapPath: return "BitmapHeapPath";
		case T_BitmapAndPath: return "BitmapAndPath";
		case T_BitmapOrPath: return "BitmapOrPath";
		case T_TidPath: return "TidPath";
		case T_TidRangePath: return "TidRangePath";
		case T_SubqueryScanPath: return "SubqueryScanPath";
		case T_ForeignPath: return "ForeignPath";
		case T_CustomPath: return "CustomPath";
		case T_AppendPath: return "AppendPath";
		case T_MergeAppendPath: return "MergeAppendPath";
		case T_GroupResultPath: return "GroupResultPath";
		case T_MaterialPath: return "MaterialPath";
		case T_MemoizePath: return "MemoizePath";
		case T_UniquePath: return "UniquePath";
		case T_GatherPath: return "GatherPath";
		case T_GatherMergePath: return "GatherMergePath";
		case T_NestPath: return "NestPath";
		case T_MergePath: return "MergePath";
		case T_HashPath: return "HashPath";
		case T_ProjectionPath: return "ProjectionPath";
		case T_ProjectSetPath: return "ProjectSetPath";
		case T_SortPath: return "SortPath";
		case T_IncrementalSortPath: return "IncrementalSortPath";
		case T_GroupPath: return "GroupPath";
		case T_UpperUniquePath: return "UpperUniquePath";
		case T_AggPath: return "AggPath";
		case T_GroupingSetsPath: return "GroupingSetsPath";
		case T_MinMaxAggPath: return "MinMaxAggPath";
		case T_WindowAggPath: return "WindowAggPath";
		case T_SetOpPath: return "SetOpPath";
		case T_RecursiveUnionPath: return "RecursiveUnionPath";
		case T_LockRowsPath: return "LockRowsPath";
		case T_ModifyTablePath: return "ModifyTablePath";
		case T_LimitPath: return "LimitPath";
		default: return "UnknownPath";
	}
}

static void
plan_replay_append_node_dump(StringInfo output, const void *node)
{
	char *dump;

	if (node == NULL)
	{
		appendStringInfoString(output, "null");
		return;
	}
	dump = nodeToString(node);
	escape_json(output, dump);
	pfree(dump);
}

static void
plan_replay_append_relids(StringInfo output, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while ((member = bms_next_member(relids, member)) >= 0)
	{
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "%d", member);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_relation_aliases(StringInfo output, PlannerInfo *root, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while ((member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte;

		if (member <= 0 || member >= root->simple_rel_array_size)
			continue;
		rte = planner_rt_fetch(member, root);
		if (rte == NULL || rte->eref == NULL)
			continue;
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		escape_json(output, rte->eref->aliasname);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_expression_text(StringInfo output, PlannerInfo *root, const Node *expr)
{
	if (expr == NULL)
	{
		appendStringInfoString(output, "NULL");
		return;
	}

	if (IsA(expr, Var))
	{
		const Var *var = (const Var *) expr;

		if (!IS_SPECIAL_VARNO(var->varno) && var->varno > 0 &&
			var->varno < root->simple_rel_array_size)
		{
			RangeTblEntry *rte = planner_rt_fetch(var->varno, root);

			if (rte != NULL && rte->eref != NULL)
			{
				appendStringInfo(output, "%s.%s", rte->eref->aliasname,
								 get_rte_attribute_name(rte, var->varattno));
				return;
			}
		}
		appendStringInfo(output, "var(%u,%d)", var->varno, var->varattno);
	}
	else if (IsA(expr, Const))
	{
		const Const *constant = (const Const *) expr;

		if (constant->constisnull)
			appendStringInfoString(output, "NULL");
		else
		{
			Oid output_function;
			bool type_is_varlena;
			char *value;

			getTypeOutputInfo(constant->consttype, &output_function,
							  &type_is_varlena);
			value = OidOutputFunctionCall(output_function, constant->constvalue);
			appendStringInfoString(output, value);
			pfree(value);
		}
	}
	else if (IsA(expr, OpExpr))
	{
		const OpExpr *op = (const OpExpr *) expr;
		char *name = get_opname(op->opno);

		appendStringInfoChar(output, '(');
		if (list_length(op->args) == 2)
		{
			plan_replay_append_expression_text(output, root, linitial(op->args));
			appendStringInfo(output, " %s ", name != NULL ? name : "?");
			plan_replay_append_expression_text(output, root, lsecond(op->args));
		}
		else
		{
			appendStringInfo(output, "%s ", name != NULL ? name : "?");
			if (op->args != NIL)
				plan_replay_append_expression_text(output, root, linitial(op->args));
		}
		appendStringInfoChar(output, ')');
		if (name != NULL)
			pfree(name);
	}
	else if (IsA(expr, ScalarArrayOpExpr))
	{
		const ScalarArrayOpExpr *op = (const ScalarArrayOpExpr *) expr;
		char *name = get_opname(op->opno);

		appendStringInfoChar(output, '(');
		plan_replay_append_expression_text(output, root, linitial(op->args));
		appendStringInfo(output, " %s %s (", name != NULL ? name : "?",
						 op->useOr ? "ANY" : "ALL");
		plan_replay_append_expression_text(output, root, lsecond(op->args));
		appendStringInfoString(output, "))");
		if (name != NULL)
			pfree(name);
	}
	else if (IsA(expr, FuncExpr))
	{
		const FuncExpr *function = (const FuncExpr *) expr;
		char *name = get_func_name(function->funcid);
		ListCell *cell;
		bool first = true;

		appendStringInfo(output, "%s(", name != NULL ? name : "?");
		foreach(cell, function->args)
		{
			if (!first)
				appendStringInfoString(output, ", ");
			first = false;
			plan_replay_append_expression_text(output, root, lfirst(cell));
		}
		appendStringInfoChar(output, ')');
		if (name != NULL)
			pfree(name);
	}
	else if (IsA(expr, BoolExpr))
	{
		const BoolExpr *boolean = (const BoolExpr *) expr;
		const char *separator = boolean->boolop == AND_EXPR ? " AND " : " OR ";
		ListCell *cell;
		bool first = true;

		if (boolean->boolop == NOT_EXPR)
			appendStringInfoString(output, "NOT ");
		appendStringInfoChar(output, '(');
		foreach(cell, boolean->args)
		{
			if (!first)
				appendStringInfoString(output, separator);
			first = false;
			plan_replay_append_expression_text(output, root, lfirst(cell));
		}
		appendStringInfoChar(output, ')');
	}
	else if (IsA(expr, RelabelType))
		plan_replay_append_expression_text(output, root,
							   (Node *) ((const RelabelType *) expr)->arg);
	else if (IsA(expr, NullTest))
	{
		const NullTest *test = (const NullTest *) expr;

		appendStringInfoChar(output, '(');
		plan_replay_append_expression_text(output, root, (Node *) test->arg);
		appendStringInfoString(output,
							 test->nulltesttype == IS_NULL ? " IS NULL)" :
							 " IS NOT NULL)");
	}
	else
	{
		char *dump = nodeToString(expr);

		appendStringInfoString(output, dump);
		pfree(dump);
	}
}

static void
plan_replay_append_restrict_clauses(StringInfo output, PlannerInfo *root, List *clauses)
{
	ListCell *cell;
	bool first = true;

	appendStringInfoChar(output, '[');
	foreach(cell, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, cell);
		StringInfoData expression;

		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		initStringInfo(&expression);
		plan_replay_append_expression_text(&expression, root, (Node *) rinfo->clause);
		appendStringInfoString(output, "{\"expression\":");
		escape_json(output, expression.data);
		appendStringInfoString(output, ",\"raw\":");
		plan_replay_append_node_dump(output, rinfo->clause);
		appendStringInfoString(output, ",\"required_relids\":");
		plan_replay_append_relids(output, rinfo->required_relids);
		appendStringInfoChar(output, '}');
		pfree(expression.data);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_compact_relations(StringInfo output, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while (plan_replay_planner_root != NULL &&
		   (member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(member, plan_replay_planner_root);

		if (rte == NULL)
			continue;
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "{\"rti\":%d,\"alias\":", member);
		if (rte->eref != NULL)
			escape_json(output, rte->eref->aliasname);
		else
			appendStringInfoString(output, "null");
		if (rte->rtekind == RTE_RELATION)
		{
			char *name = get_rel_name(rte->relid);
			char *schema = get_namespace_name(get_rel_namespace(rte->relid));

			appendStringInfoString(output, ",\"schema\":");
			if (schema != NULL)
				escape_json(output, schema);
			else
				appendStringInfoString(output, "null");
			appendStringInfoString(output, ",\"table\":");
			if (name != NULL)
				escape_json(output, name);
			else
				appendStringInfoString(output, "null");
			if (schema != NULL)
				pfree(schema);
			if (name != NULL)
				pfree(name);
		}
		appendStringInfoChar(output, '}');
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_child(StringInfo output, Path *child, bool *first)
{
	if (child == NULL)
		return;
	if (!*first)
		appendStringInfoChar(output, ',');
	*first = false;
	plan_replay_append_path_json(output, child);
}

static void
plan_replay_append_path_list(StringInfo output, List *paths, bool *first)
{
	ListCell *cell;

	foreach(cell, paths)
		plan_replay_append_child(output, (Path *) lfirst(cell), first);
}

static void
plan_replay_append_compact_path_json(StringInfo output, Path *path)
{
	bool first_child = true;

	appendStringInfoChar(output, '{');
	appendStringInfoString(output, "\"op_name\":");
	escape_json(output, plan_replay_operator_name(path));
	appendStringInfo(output,
					 ",\"startup_cost\":%.17g,\"total_cost\":%.17g"
					 ",\"rows\":%.17g,\"width\":%d,\"pathkeys\":%d"
					 ",\"parallel_aware\":%s,\"parallel_workers\":%d",
					 path->startup_cost, path->total_cost,
					 path->rows,
					 path->pathtarget != NULL ? path->pathtarget->width : 0,
					 list_length(path->pathkeys),
					 path->parallel_aware ? "true" : "false",
					 path->parallel_workers);
	appendStringInfoString(output, ",\"relids\":");
	plan_replay_append_relids(output, path->parent != NULL ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relations\":");
	plan_replay_append_compact_relations(output,
								 path->parent != NULL ? path->parent->relids : NULL);

	if (IsA(path, IndexPath) && ((IndexPath *) path)->indexinfo != NULL)
	{
		char *index_name = get_rel_name(((IndexPath *) path)->indexinfo->indexoid);

		if (index_name != NULL)
		{
			appendStringInfoString(output, ",\"index_name\":");
			escape_json(output, index_name);
			pfree(index_name);
		}
	}
	if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath))
	{
		appendStringInfoString(output, ",\"join_type\":");
		escape_json(output, plan_replay_join_type_name(((JoinPath *) path)->jointype));
	}

	appendStringInfoString(output, ",\"children\":[");
	switch (nodeTag(path))
	{
		case T_BitmapHeapPath:
			plan_replay_append_child(output, ((BitmapHeapPath *) path)->bitmapqual, &first_child);
			break;
		case T_BitmapAndPath:
			plan_replay_append_path_list(output, ((BitmapAndPath *) path)->bitmapquals, &first_child);
			break;
		case T_BitmapOrPath:
			plan_replay_append_path_list(output, ((BitmapOrPath *) path)->bitmapquals, &first_child);
			break;
		case T_SubqueryScanPath:
			plan_replay_append_child(output, ((SubqueryScanPath *) path)->subpath, &first_child);
			break;
		case T_ForeignPath:
			plan_replay_append_child(output, ((ForeignPath *) path)->fdw_outerpath, &first_child);
			break;
		case T_CustomPath:
			plan_replay_append_path_list(output, ((CustomPath *) path)->custom_paths, &first_child);
			break;
		case T_AppendPath:
			plan_replay_append_path_list(output, ((AppendPath *) path)->subpaths, &first_child);
			break;
		case T_MergeAppendPath:
			plan_replay_append_path_list(output, ((MergeAppendPath *) path)->subpaths, &first_child);
			break;
		case T_MaterialPath:
			plan_replay_append_child(output, ((MaterialPath *) path)->subpath, &first_child);
			break;
		case T_MemoizePath:
			plan_replay_append_child(output, ((MemoizePath *) path)->subpath, &first_child);
			break;
		case T_UniquePath:
			plan_replay_append_child(output, ((UniquePath *) path)->subpath, &first_child);
			break;
		case T_GatherPath:
			plan_replay_append_child(output, ((GatherPath *) path)->subpath, &first_child);
			break;
		case T_GatherMergePath:
			plan_replay_append_child(output, ((GatherMergePath *) path)->subpath, &first_child);
			break;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			plan_replay_append_child(output, ((JoinPath *) path)->outerjoinpath, &first_child);
			plan_replay_append_child(output, ((JoinPath *) path)->innerjoinpath, &first_child);
			break;
		case T_ProjectionPath:
			plan_replay_append_child(output, ((ProjectionPath *) path)->subpath, &first_child);
			break;
		case T_ProjectSetPath:
			plan_replay_append_child(output, ((ProjectSetPath *) path)->subpath, &first_child);
			break;
		case T_SortPath:
		case T_IncrementalSortPath:
			plan_replay_append_child(output, ((SortPath *) path)->subpath, &first_child);
			break;
		case T_GroupPath:
			plan_replay_append_child(output, ((GroupPath *) path)->subpath, &first_child);
			break;
		case T_UpperUniquePath:
			plan_replay_append_child(output, ((UpperUniquePath *) path)->subpath, &first_child);
			break;
		case T_AggPath:
			plan_replay_append_child(output, ((AggPath *) path)->subpath, &first_child);
			break;
		case T_GroupingSetsPath:
			plan_replay_append_child(output, ((GroupingSetsPath *) path)->subpath, &first_child);
			break;
		case T_WindowAggPath:
			plan_replay_append_child(output, ((WindowAggPath *) path)->subpath, &first_child);
			break;
		case T_SetOpPath:
			plan_replay_append_child(output, ((SetOpPath *) path)->leftpath, &first_child);
			plan_replay_append_child(output, ((SetOpPath *) path)->rightpath, &first_child);
			break;
		case T_RecursiveUnionPath:
			plan_replay_append_child(output, ((RecursiveUnionPath *) path)->leftpath, &first_child);
			plan_replay_append_child(output, ((RecursiveUnionPath *) path)->rightpath, &first_child);
			break;
		case T_LockRowsPath:
			plan_replay_append_child(output, ((LockRowsPath *) path)->subpath, &first_child);
			break;
		case T_ModifyTablePath:
			plan_replay_append_child(output, ((ModifyTablePath *) path)->subpath, &first_child);
			break;
		case T_LimitPath:
			plan_replay_append_child(output, ((LimitPath *) path)->subpath, &first_child);
			break;
		default:
			break;
	}
	appendStringInfoString(output, "]}");
}

static void
plan_replay_append_path_json(StringInfo output, Path *path)
{
	plan_replay_append_compact_path_json(output, path);
}

static int
plan_replay_string_compare(const void *left, const void *right)
{
	const char *const *left_string = (const char *const *) left;
	const char *const *right_string = (const char *const *) right;

	return strcmp(*left_string, *right_string);
}

static int
plan_replay_path_id_compare(const void *left, const void *right)
{
	const PlanReplayPathId *left_id = (const PlanReplayPathId *) left;
	const PlanReplayPathId *right_id = (const PlanReplayPathId *) right;

	if (left_id->hi < right_id->hi)
		return -1;
	if (left_id->hi > right_id->hi)
		return 1;
	if (left_id->lo < right_id->lo)
		return -1;
	if (left_id->lo > right_id->lo)
		return 1;
	return 0;
}

static void
plan_replay_append_descriptor_node(StringInfo output, const char *name,
								   const void *node)
{
	char *value = nodeToString(node);

	appendStringInfo(output, "|%s=%zu:", name, strlen(value));
	appendStringInfoString(output, value);
	pfree(value);
}

static void
plan_replay_append_descriptor_relations(StringInfo output, Relids relids)
{
	int rti = -1;

	appendStringInfoString(output, "|relations=[");
	while ((rti = bms_next_member(relids, rti)) >= 0)
	{
		RangeTblEntry *rte;

		if (plan_replay_planner_root == NULL ||
			rti <= 0 || rti >= plan_replay_planner_root->simple_rel_array_size)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("cannot identify relation %d for stable Path ID", rti)));
		rte = planner_rt_fetch(rti, plan_replay_planner_root);
		appendStringInfo(output, "%d:%d:%u;", rti, (int) rte->rtekind,
						 rte->rtekind == RTE_RELATION ? rte->relid : InvalidOid);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_descriptor_clause_set(StringInfo output, const char *name,
									 List *clauses, bool restrict_infos)
{
	int count = list_length(clauses);
	char **values;
	ListCell *cell;
	int index = 0;

	if (count == 0)
	{
		appendStringInfo(output, "|%s=[]", name);
		return;
	}
	values = palloc(sizeof(char *) * count);
	foreach(cell, clauses)
	{
		Node *node = (Node *) lfirst(cell);

		if (restrict_infos)
		{
			if (!IsA(node, RestrictInfo))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("expected RestrictInfo while building stable Path ID")));
			node = (Node *) ((RestrictInfo *) node)->clause;
		}
		values[index++] = nodeToString(node);
	}
	qsort(values, count, sizeof(char *), plan_replay_string_compare);
	appendStringInfo(output, "|%s=[", name);
	for (index = 0; index < count; index++)
	{
		appendStringInfo(output, "%zu:", strlen(values[index]));
		appendStringInfoString(output, values[index]);
		appendStringInfoChar(output, ';');
		pfree(values[index]);
	}
	appendStringInfoChar(output, ']');
	pfree(values);
}

static void
plan_replay_append_descriptor_index_clauses(StringInfo output, List *clauses)
{
	int count = list_length(clauses);
	char **values;
	ListCell *cell;
	int index = 0;

	if (count == 0)
	{
		appendStringInfoString(output, "|index_clauses=[]");
		return;
	}
	values = palloc(sizeof(char *) * count);
	foreach(cell, clauses)
	{
		IndexClause *clause = (IndexClause *) lfirst(cell);
		StringInfoData value;

		if (!IsA(clause, IndexClause))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("expected IndexClause while building stable Path ID")));
		initStringInfo(&value);
		appendStringInfo(&value, "col=%d|lossy=%d", clause->indexcol,
						 clause->lossy ? 1 : 0);
		plan_replay_append_descriptor_node(&value, "indexcols", clause->indexcols);
		plan_replay_append_descriptor_node(&value, "clause", clause->rinfo->clause);
		plan_replay_append_descriptor_clause_set(&value, "indexquals",
											 clause->indexquals, true);
		values[index++] = value.data;
	}
	qsort(values, count, sizeof(char *), plan_replay_string_compare);
	appendStringInfoString(output, "|index_clauses=[");
	for (index = 0; index < count; index++)
	{
		appendStringInfo(output, "%zu:", strlen(values[index]));
		appendStringInfoString(output, values[index]);
		appendStringInfoChar(output, ';');
		pfree(values[index]);
	}
	appendStringInfoChar(output, ']');
	pfree(values);
}

static void
plan_replay_format_path_id(PlanReplayPathId id, char *output)
{
	snprintf(output, PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1,
			 "%016" PRIx64 "%016" PRIx64, id.hi, id.lo);
}

static PlanReplayPathId
plan_replay_hash_descriptor(const char *descriptor)
{
	pg_cryptohash_ctx *context;
	uint8 digest[PG_SHA256_DIGEST_LENGTH];
	PlanReplayPathId id = {0, 0};
	int index;

	context = pg_cryptohash_create(PG_SHA256);
	if (context == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not create SHA-256 context for stable Path ID")));
	if (pg_cryptohash_init(context) < 0 ||
		pg_cryptohash_update(context, (const uint8 *) descriptor,
							 strlen(descriptor)) < 0 ||
		pg_cryptohash_final(context, digest, sizeof(digest)) < 0)
	{
		char *message = pstrdup(pg_cryptohash_error(context));

		pg_cryptohash_free(context);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not hash stable Path descriptor: %s", message)));
	}
	pg_cryptohash_free(context);
	for (index = 0; index < 8; index++)
		id.hi = (id.hi << 8) | digest[index];
	for (index = 8; index < PLAN_REPLAY_PATH_ID_BYTES; index++)
		id.lo = (id.lo << 8) | digest[index];
	return id;
}

static void
plan_replay_register_descriptor(PlanReplayPathId id, const char *descriptor)
{
	PlanReplayIdentityEntry *entry;
	bool found;

	entry = hash_search(plan_replay_identity_table, &id, HASH_ENTER, &found);
	if (!found)
	{
		entry->descriptor = pstrdup(descriptor);
		return;
	}
	if (strcmp(entry->descriptor, descriptor) != 0)
	{
		char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

		plan_replay_format_path_id(id, path_id);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("stable Path ID collision for %s", path_id),
				 errdetail("Two different canonical Path descriptors produced the same 128-bit ID.")));
	}
}

static PlanReplayPathId
plan_replay_child_path_id(Path *child)
{
	if (child == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("stable Path ID requires a non-null child Path")));
	return plan_replay_compute_path_id(child);
}

static void
plan_replay_append_descriptor_child(StringInfo output, const char *name,
								Path *child)
{
	PlanReplayPathId id = plan_replay_child_path_id(child);
	char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

	plan_replay_format_path_id(id, path_id);
	appendStringInfo(output, "|%s=%s", name, path_id);
}

static void
plan_replay_append_descriptor_bitmap_children(StringInfo output, List *children)
{
	int count = list_length(children);
	PlanReplayPathId *ids;
	ListCell *cell;
	int index = 0;

	ids = palloc(sizeof(PlanReplayPathId) * Max(count, 1));
	foreach(cell, children)
		ids[index++] = plan_replay_child_path_id((Path *) lfirst(cell));
	qsort(ids, count, sizeof(PlanReplayPathId), plan_replay_path_id_compare);
	appendStringInfoString(output, "|bitmap_children=[");
	for (index = 0; index < count; index++)
	{
		char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

		plan_replay_format_path_id(ids[index], path_id);
		appendStringInfo(output, "%s;", path_id);
	}
	appendStringInfoChar(output, ']');
	pfree(ids);
}

static void
plan_replay_append_descriptor_append_children(StringInfo output, AppendPath *append)
{
	ListCell *cell;
	appendStringInfoString(output, "|append_children=[");
	foreach(cell, append->subpaths)
	{
		PlanReplayPathId id = plan_replay_child_path_id((Path *) lfirst(cell));
		char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

		plan_replay_format_path_id(id, path_id);
		appendStringInfo(output, "%s;", path_id);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_descriptor_path_list(StringInfo output, const char *name,
								List *children)
{
	ListCell *cell;

	appendStringInfo(output, "|%s=[", name);
	foreach(cell, children)
	{
		PlanReplayPathId id = plan_replay_child_path_id((Path *) lfirst(cell));
		char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

		plan_replay_format_path_id(id, path_id);
		appendStringInfo(output, "%s;", path_id);
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_descriptor_target_sortgrouprefs(StringInfo output,
												PathTarget *target)
{
	int count = list_length(target->exprs);
	int index;

	appendStringInfoString(output, "|target_sortgrouprefs=[");
	for (index = 0; index < count; index++)
		appendStringInfo(output, "%u;",
						 target->sortgrouprefs != NULL ?
						 target->sortgrouprefs[index] : 0);
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_path_descriptor(StringInfo output, Path *path)
{
	Relids required_outer = PATH_REQ_OUTER(path);
	Relids parent_relids = path->parent != NULL ? path->parent->relids : NULL;

	if (nodeTag(path) == T_Path && path->pathtype != T_SeqScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Path type %s is not supported by stable Path ID format 1",
						plan_replay_path_node_name(path)),
				 errdetail("Cost capture and replay fail closed instead of falling back to sequence matching.")));

	appendStringInfo(output,
				 "pg_plan_replay_path_id_v1|node_tag=%d|pathtype=%d"
				 "|parallel_aware=%d|parallel_safe=%d|parallel_workers=%d",
				 (int) nodeTag(path), (int) path->pathtype,
				 path->parallel_aware ? 1 : 0, path->parallel_safe ? 1 : 0,
				 path->parallel_workers);
	appendStringInfoString(output, "|parent_relids=");
	plan_replay_append_relids(output, parent_relids);
	plan_replay_append_descriptor_relations(output, parent_relids);
	appendStringInfoString(output, "|required_outer=");
	plan_replay_append_relids(output, required_outer);
	plan_replay_append_descriptor_node(output, "pathkeys", path->pathkeys);
	if (path->parent != NULL && path->parent->relid > 0)
		plan_replay_append_descriptor_clause_set(output, "base_restrictions",
											 path->parent->baserestrictinfo, true);
	if (path->pathtarget != NULL)
	{
		plan_replay_append_descriptor_node(output, "target_exprs",
									  path->pathtarget->exprs);
		plan_replay_append_descriptor_target_sortgrouprefs(output,
													 path->pathtarget);
	}
	else
		appendStringInfoString(output, "|target=null");

	switch (nodeTag(path))
	{
		case T_Path:
			break;
		case T_IndexPath:
		{
			IndexPath *index_path = (IndexPath *) path;

			if (index_path->indexinfo == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("IndexPath has no index while building stable Path ID")));
			appendStringInfo(output, "|index_oid=%u|scan_direction=%d",
							 index_path->indexinfo->indexoid,
							 (int) index_path->indexscandir);
			plan_replay_append_descriptor_index_clauses(output,
												 index_path->indexclauses);
			plan_replay_append_descriptor_node(output, "index_orderbys",
										  index_path->indexorderbys);
			plan_replay_append_descriptor_node(output, "index_orderbycols",
										  index_path->indexorderbycols);
			break;
		}
		case T_BitmapHeapPath:
			plan_replay_append_descriptor_child(output, "bitmapqual",
										((BitmapHeapPath *) path)->bitmapqual);
			break;
		case T_BitmapAndPath:
			plan_replay_append_descriptor_bitmap_children(output,
												((BitmapAndPath *) path)->bitmapquals);
			break;
		case T_BitmapOrPath:
			plan_replay_append_descriptor_bitmap_children(output,
												((BitmapOrPath *) path)->bitmapquals);
			break;
		case T_AppendPath:
			plan_replay_append_descriptor_append_children(output,
												(AppendPath *) path);
			break;
		case T_MergeAppendPath:
			plan_replay_append_descriptor_path_list(output, "merge_append_children",
											 ((MergeAppendPath *) path)->subpaths);
			break;
		case T_GroupResultPath:
			plan_replay_append_descriptor_clause_set(output, "quals",
											 ((GroupResultPath *) path)->quals, false);
			break;
		case T_MaterialPath:
			plan_replay_append_descriptor_child(output, "subpath",
										((MaterialPath *) path)->subpath);
			break;
		case T_MemoizePath:
		{
			MemoizePath *memoize = (MemoizePath *) path;

			appendStringInfo(output, "|single_row=%d|binary_mode=%d",
							 memoize->singlerow ? 1 : 0,
							 memoize->binary_mode ? 1 : 0);
			plan_replay_append_descriptor_node(output, "hash_operators",
										  memoize->hash_operators);
			plan_replay_append_descriptor_node(output, "parameter_exprs",
										  memoize->param_exprs);
			plan_replay_append_descriptor_child(output, "subpath",
										memoize->subpath);
			break;
		}
		case T_UniquePath:
		{
			UniquePath *unique = (UniquePath *) path;

			appendStringInfo(output, "|unique_method=%d", (int) unique->umethod);
			plan_replay_append_descriptor_node(output, "unique_operators",
										  unique->in_operators);
			plan_replay_append_descriptor_node(output, "unique_exprs",
										  unique->uniq_exprs);
			plan_replay_append_descriptor_child(output, "subpath",
										unique->subpath);
			break;
		}
		case T_GatherPath:
		{
			GatherPath *gather = (GatherPath *) path;

			appendStringInfo(output, "|single_copy=%d|workers=%d",
							 gather->single_copy ? 1 : 0, gather->num_workers);
			plan_replay_append_descriptor_child(output, "subpath",
										gather->subpath);
			break;
		}
		case T_GatherMergePath:
		{
			GatherMergePath *gather = (GatherMergePath *) path;

			appendStringInfo(output, "|workers=%d", gather->num_workers);
			plan_replay_append_descriptor_child(output, "subpath",
										gather->subpath);
			break;
		}
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
		{
			JoinPath *join = (JoinPath *) path;

			appendStringInfo(output, "|join_type=%d|inner_unique=%d",
							 (int) join->jointype, join->inner_unique ? 1 : 0);
			plan_replay_append_descriptor_clause_set(output, "join_clauses",
											 join->joinrestrictinfo, true);
			if (IsA(path, MergePath))
			{
				MergePath *merge = (MergePath *) path;

				plan_replay_append_descriptor_clause_set(output, "merge_clauses",
												 merge->path_mergeclauses, true);
				plan_replay_append_descriptor_node(output, "outer_sortkeys",
											  merge->outersortkeys);
				plan_replay_append_descriptor_node(output, "inner_sortkeys",
											  merge->innersortkeys);
				appendStringInfo(output,
							 "|outer_presorted=%d|skip_mark_restore=%d|materialize_inner=%d",
							 merge->outer_presorted_keys,
							 merge->skip_mark_restore ? 1 : 0,
							 merge->materialize_inner ? 1 : 0);
			}
			else if (IsA(path, HashPath))
				plan_replay_append_descriptor_clause_set(output, "hash_clauses",
												 ((HashPath *) path)->path_hashclauses,
												 true);
			plan_replay_append_descriptor_child(output, "outer",
										join->outerjoinpath);
			plan_replay_append_descriptor_child(output, "inner",
										join->innerjoinpath);
			break;
		}
		case T_ProjectionPath:
			appendStringInfo(output, "|dummy=%d",
							 ((ProjectionPath *) path)->dummypp ? 1 : 0);
			plan_replay_append_descriptor_child(output, "subpath",
										((ProjectionPath *) path)->subpath);
			break;
		case T_SortPath:
			plan_replay_append_descriptor_child(output, "subpath",
										((SortPath *) path)->subpath);
			break;
		case T_IncrementalSortPath:
			appendStringInfo(output, "|presorted_columns=%d",
							 ((IncrementalSortPath *) path)->nPresortedCols);
			plan_replay_append_descriptor_child(output, "subpath",
										((SortPath *) path)->subpath);
			break;
		case T_GroupPath:
			plan_replay_append_descriptor_node(output, "group_clause",
										  ((GroupPath *) path)->groupClause);
			plan_replay_append_descriptor_clause_set(output, "having_quals",
											 ((GroupPath *) path)->qual, false);
			plan_replay_append_descriptor_child(output, "subpath",
										((GroupPath *) path)->subpath);
			break;
		case T_UpperUniquePath:
			appendStringInfo(output, "|keys=%d",
							 ((UpperUniquePath *) path)->numkeys);
			plan_replay_append_descriptor_child(output, "subpath",
										((UpperUniquePath *) path)->subpath);
			break;
		case T_AggPath:
		{
			AggPath *agg = (AggPath *) path;

			appendStringInfo(output, "|strategy=%d|split=%d",
							 (int) agg->aggstrategy, (int) agg->aggsplit);
			plan_replay_append_descriptor_node(output, "group_clause",
										  agg->groupClause);
			plan_replay_append_descriptor_clause_set(output, "having_quals",
											 agg->qual, false);
			plan_replay_append_descriptor_child(output, "subpath",
										agg->subpath);
			break;
		}
		case T_LimitPath:
		{
			LimitPath *limit = (LimitPath *) path;

			appendStringInfo(output, "|limit_option=%d", (int) limit->limitOption);
			plan_replay_append_descriptor_node(output, "limit_offset",
										  limit->limitOffset);
			plan_replay_append_descriptor_node(output, "limit_count",
										  limit->limitCount);
			plan_replay_append_descriptor_child(output, "subpath",
										limit->subpath);
			break;
		}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Path type %s is not supported by stable Path ID format 1",
							plan_replay_path_node_name(path)),
					 errdetail("Cost capture and replay fail closed instead of falling back to sequence matching.")));
	}
}

static PlanReplayPathId
plan_replay_compute_path_id(Path *path)
{
	StringInfoData descriptor;
	PlanReplayPathId id;

	if (path->plan_replay_path_id_valid)
	{
		id.hi = path->plan_replay_path_id_hi;
		id.lo = path->plan_replay_path_id_lo;
		return id;
	}
	initStringInfo(&descriptor);
	plan_replay_append_path_descriptor(&descriptor, path);
	id = plan_replay_hash_descriptor(descriptor.data);
	plan_replay_register_descriptor(id, descriptor.data);
	path->plan_replay_path_id_valid = true;
	path->plan_replay_path_id_hi = id.hi;
	path->plan_replay_path_id_lo = id.lo;
	pfree(descriptor.data);
	return id;
}

static void
plan_replay_capture_path(RelOptInfo *rel, Path *path, bool partial,
					 Cost native_startup_cost, Cost native_total_cost,
					 bool replay_applied, bool model_applied)
{
	StringInfoData line;
	PlanReplayPathId id;
	char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];
	size_t written;

	initStringInfo(&line);
	Assert(path->plan_replay_path_id_valid);
	id.hi = path->plan_replay_path_id_hi;
	id.lo = path->plan_replay_path_id_lo;
	plan_replay_format_path_id(id, path_id);
	appendStringInfo(&line,
					 "{\"format_version\":6,\"sequence\":" UINT64_FORMAT,
					 plan_replay_path_sequence);
	appendStringInfoString(&line, ",\"path_id\":");
	escape_json(&line, path_id);
	appendStringInfo(&line,
					 ",\"cost_replay_applied\":%s,"
					 "\"model_prediction_applied\":%s,\"cost_source\":\"%s\","
					 "\"native_startup_cost\":%.17g,\"native_total_cost\":%.17g,"
					 "\"effective_startup_cost\":%.17g,\"effective_total_cost\":%.17g",
					 replay_applied ? "true" : "false",
					 model_applied ? "true" : "false",
					 model_applied ? "model" : replay_applied ? "file" : "native",
					 native_startup_cost, native_total_cost,
					 path->startup_cost, path->total_cost);
	appendStringInfoString(&line, ",\"cardinality_sequence\":");
	if (path->plan_replay_cardinality_sequence_valid)
		appendStringInfo(&line, UINT64_FORMAT,
						 path->plan_replay_cardinality_sequence);
	else
		appendStringInfoString(&line, "null");
	appendStringInfo(&line, ",\"partial\":%s,\"relids\":",
						 partial ? "true" : "false");
	plan_replay_append_relids(&line, rel->relids);
	appendStringInfoString(&line, ",\"path\":");
	plan_replay_append_path_json(&line, path);
	appendStringInfoString(&line, "}\n");

	written = fwrite(line.data, 1, line.len, plan_replay_path_capture_stream);
	if (written != (size_t) line.len)
		ereport(ERROR,
				(errcode_for_file_access(),
					 errmsg("could not write pg_plan_replay capture file: %m")));
	pfree(line.data);
}

static int
plan_replay_hex_value(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static bool
plan_replay_parse_path_id(const char *text, PlanReplayPathId *id)
{
	int index;

	id->hi = 0;
	id->lo = 0;
	for (index = 0; index < PLAN_REPLAY_PATH_ID_HEX_LENGTH; index++)
	{
		int value = plan_replay_hex_value(text[index]);

		if (value < 0)
			return false;
		if (index < 16)
			id->hi = (id->hi << 4) | value;
		else
			id->lo = (id->lo << 4) | value;
	}
	return text[PLAN_REPLAY_PATH_ID_HEX_LENGTH] == '\0' ||
		isspace((unsigned char) text[PLAN_REPLAY_PATH_ID_HEX_LENGTH]);
}

static void
plan_replay_load_costs(void)
{
	char line[PLAN_REPLAY_COST_LINE_MAX];
	uint64 line_number = 0;
	bool header_seen = false;

	Assert(plan_replay_path_replay_stream != NULL);
	Assert(plan_replay_cost_table != NULL);
	while (fgets(line, sizeof(line), plan_replay_path_replay_stream) != NULL)
	{
		char *cursor = line;
		char *end;
		PlanReplayPathId id;
		PlanReplayCostEntry *entry;
		double startup_cost;
		double total_cost;
		bool found;

		line_number++;
		if (strchr(line, '\n') == NULL && !feof(plan_replay_path_replay_stream))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay cost replay line " UINT64_FORMAT " is too long",
							line_number)));
		while (*cursor != '\0' && isspace((unsigned char) *cursor))
			cursor++;
		if (*cursor == '#')
		{
			char *newline = strchr(cursor, '\n');

			if (newline != NULL)
				*newline = '\0';
			if (strcmp(cursor, "# pg_plan_replay_costs format=2") != 0 ||
				header_seen || line_number != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid pg_plan_replay cost replay header at line "
								UINT64_FORMAT, line_number),
						 errdetail("Expected exactly: # pg_plan_replay_costs format=2")));
			header_seen = true;
			continue;
		}
		if (*cursor == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("empty pg_plan_replay cost replay line " UINT64_FORMAT,
							line_number)));
		if (!header_seen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay cost replay file has no format 2 header"),
					 errhint("Regenerate the sparse .costs file; sequence-based files are not supported.")));

		if (strlen(cursor) < PLAN_REPLAY_PATH_ID_HEX_LENGTH ||
			!plan_replay_parse_path_id(cursor, &id))
			goto invalid_line;
		cursor += PLAN_REPLAY_PATH_ID_HEX_LENGTH;
		if (!isspace((unsigned char) *cursor))
			goto invalid_line;
		while (isspace((unsigned char) *cursor))
			cursor++;

		errno = 0;
		startup_cost = strtod(cursor, &end);
		if (cursor == end || errno == ERANGE)
			goto invalid_line;
		cursor = end;
		errno = 0;
		total_cost = strtod(cursor, &end);
		if (cursor == end || errno == ERANGE)
			goto invalid_line;
		cursor = end;
		while (*cursor != '\0' && isspace((unsigned char) *cursor))
			cursor++;
		if (*cursor != '\0')
			goto invalid_line;
		if (!isfinite(startup_cost) || !isfinite(total_cost) ||
			startup_cost < 0.0 || total_cost < 0.0 ||
			startup_cost > total_cost)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid pg_plan_replay costs at line " UINT64_FORMAT,
							line_number),
					 errdetail("Costs must be finite and nonnegative with startup_cost <= total_cost.")));

		entry = hash_search(plan_replay_cost_table, &id, HASH_ENTER, &found);
		if (found)
		{
			char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

			plan_replay_format_path_id(id, path_id);
			ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("duplicate stable Path ID %s in cost replay file", path_id)));
		}
		entry->startup_cost = startup_cost;
		entry->total_cost = total_cost;
		entry->matched = false;
		plan_replay_cost_replay_entry_count++;
		continue;

invalid_line:
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("malformed pg_plan_replay cost replay line " UINT64_FORMAT,
						line_number),
				 errdetail("Expected exactly: 32_hex_path_id startup_cost total_cost")));
	}
	if (ferror(plan_replay_path_replay_stream))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read pg_plan_replay Path replay file: %m")));
	if (!header_seen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay cost replay file is empty"),
				 errdetail("Expected header: # pg_plan_replay_costs format=2")));
}

static bool
plan_replay_replay_path(Path *path)
{
	PlanReplayPathId id;
	PlanReplayCostEntry *entry;

	Assert(path->plan_replay_path_id_valid);
	id.hi = path->plan_replay_path_id_hi;
	id.lo = path->plan_replay_path_id_lo;
	entry = hash_search(plan_replay_cost_table, &id, HASH_FIND, NULL);
	if (entry == NULL)
		return false;

	path->startup_cost = entry->startup_cost;
	path->total_cost = entry->total_cost;
	if (!entry->matched)
	{
		entry->matched = true;
		plan_replay_cost_replay_matched_entry_count++;
	}
	plan_replay_cost_replay_applied_count++;
	return true;
}

static const char *
plan_replay_cardinality_kind_name(CardinalityEstimateKind kind)
{
	switch (kind)
	{
		case CARDINALITY_ESTIMATE_BASE: return "base";
		case CARDINALITY_ESTIMATE_JOIN: return "join";
		case CARDINALITY_ESTIMATE_PARAMETERIZED_JOIN:
			return "parameterized_join";
	}
	return "unknown";
}

static const char *
plan_replay_join_type_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER: return "inner";
		case JOIN_LEFT: return "left";
		case JOIN_FULL: return "full";
		case JOIN_RIGHT: return "right";
		case JOIN_SEMI: return "semi";
		case JOIN_ANTI: return "anti";
		case JOIN_RIGHT_SEMI: return "right_semi";
		case JOIN_RIGHT_ANTI: return "right_anti";
		case JOIN_UNIQUE_OUTER: return "unique_outer";
		case JOIN_UNIQUE_INNER: return "unique_inner";
	}
	return "unknown";
}

static void
plan_replay_capture_cardinality(PlannerInfo *root, CardinalityEstimateKind kind,
						RelOptInfo *rel, RelOptInfo *outer_rel,
						RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo,
						List *restrictlist, Cardinality outer_rows,
						Cardinality inner_rows, Cardinality native_rows,
						Cardinality effective_rows)
{
	StringInfoData line;
	size_t written;

	initStringInfo(&line);
	appendStringInfo(&line,
					 "{\"format_version\":2,\"sequence\":" UINT64_FORMAT
					 ",\"kind\":",
					 plan_replay_cardinality_sequence);
	escape_json(&line, plan_replay_cardinality_kind_name(kind));
	appendStringInfo(&line,
					 ",\"native_rows\":%.17g,\"effective_rows\":%.17g,"
					 "\"width\":%d,\"relids\":",
					 native_rows, effective_rows,
					 rel->reltarget != NULL ? rel->reltarget->width : 0);
	plan_replay_append_relids(&line, rel->relids);
	appendStringInfoString(&line, ",\"relations\":");
	plan_replay_append_relation_aliases(&line, root, rel->relids);

	if (kind == CARDINALITY_ESTIMATE_BASE)
	{
		appendStringInfo(&line, ",\"table_tuples\":%.17g,\"filter_clause\":",
						 rel->tuples);
		plan_replay_append_restrict_clauses(&line, root, restrictlist);
		appendStringInfoString(&line, ",\"join_clause\":[]");
	}
	else
	{
		appendStringInfoString(&line, ",\"outer_relids\":");
		plan_replay_append_relids(&line, outer_rel->relids);
		appendStringInfoString(&line, ",\"outer_relations\":");
		plan_replay_append_relation_aliases(&line, root, outer_rel->relids);
		appendStringInfo(&line, ",\"outer_rows\":%.17g,\"inner_relids\":",
						 outer_rows);
		plan_replay_append_relids(&line, inner_rel->relids);
		appendStringInfoString(&line, ",\"inner_relations\":");
		plan_replay_append_relation_aliases(&line, root, inner_rel->relids);
		appendStringInfo(&line, ",\"inner_rows\":%.17g,\"join_type\":",
						 inner_rows);
		escape_json(&line, plan_replay_join_type_name(sjinfo->jointype));
		appendStringInfoString(&line, ",\"filter_clause\":[],\"join_clause\":");
		plan_replay_append_restrict_clauses(&line, root, restrictlist);
	}
	appendStringInfoString(&line, "}\n");

	written = fwrite(line.data, 1, line.len, plan_replay_cardinality_capture_stream);
	if (written != (size_t) line.len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write pg_plan_replay cardinality capture file: %m")));
	pfree(line.data);
}

static Cardinality
plan_replay_replay_cardinality(CardinalityEstimateKind kind)
{
	char line[256];
	char *cursor;
	char *end;
	double rows;

	if (fgets(line, sizeof(line), plan_replay_cardinality_replay_stream) == NULL)
	{
		if (ferror(plan_replay_cardinality_replay_stream))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read pg_plan_replay cardinality replay file: %m")));
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay cardinality replay file ended before estimate "
						UINT64_FORMAT, plan_replay_cardinality_sequence),
				 errdetail("Expected rows for %s estimate.",
						   plan_replay_cardinality_kind_name(kind))));
	}
	if (strchr(line, '\n') == NULL && !feof(plan_replay_cardinality_replay_stream))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay cardinality replay line " UINT64_FORMAT
						" is too long", plan_replay_cardinality_sequence)));

	cursor = line;
	errno = 0;
	rows = strtod(cursor, &end);
	if (cursor == end || errno == ERANGE)
		goto invalid_line;
	cursor = end;
	while (*cursor != '\0' && isspace((unsigned char) *cursor))
		cursor++;
	if (*cursor != '\0')
		goto invalid_line;
	if (!isfinite(rows) || rows < 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("invalid pg_plan_replay cardinality at estimate " UINT64_FORMAT,
						plan_replay_cardinality_sequence),
				 errdetail("Rows must be finite and nonnegative.")));
	return rows;

invalid_line:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg("malformed pg_plan_replay cardinality replay line for estimate "
					UINT64_FORMAT, plan_replay_cardinality_sequence),
			 errdetail("Expected exactly one rows value.")));
}

static Cardinality
plan_replay_cardinality_estimate(PlannerInfo *root, CardinalityEstimateKind kind,
							 RelOptInfo *rel, RelOptInfo *outer_rel,
							 RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo,
							 List *restrictlist, Cardinality outer_rows,
							 Cardinality inner_rows, Cardinality native_rows,
							 bool *sequence_valid, uint64 *sequence)
{
	Cardinality rows = native_rows;
	Cardinality captured_native_rows;

	if (previous_cardinality_estimate_hook != NULL)
		rows = previous_cardinality_estimate_hook(root, kind, rel,
													  outer_rel, inner_rel, sjinfo,
													  restrictlist, outer_rows,
													  inner_rows, native_rows,
													  sequence_valid, sequence);
	if (plan_replay_cardinality_capture_stream == NULL &&
		plan_replay_cardinality_replay_stream == NULL)
		return rows;

	plan_replay_planner_root = root;
	captured_native_rows = rows;
	if (plan_replay_cardinality_replay_stream != NULL)
		rows = plan_replay_replay_cardinality(kind);
	if (plan_replay_cardinality_capture_stream != NULL)
		plan_replay_capture_cardinality(root, kind, rel, outer_rel, inner_rel, sjinfo,
									restrictlist, outer_rows, inner_rows,
									captured_native_rows, rows);
	*sequence_valid = true;
	*sequence = plan_replay_cardinality_sequence;
	plan_replay_cardinality_sequence++;
	return rows;
}

static bool
plan_replay_is_active(void)
{
	int path_mode = plan_replay_effective_channel_mode(plan_replay_path_mode);

	return plan_replay_path_capture_stream != NULL ||
		plan_replay_path_replay_stream != NULL ||
		plan_replay_mode_predicts(path_mode);
}

static void
plan_replay_process_paths(RelOptInfo *rel, List *paths, bool partial)
{
	ListCell *cell;
	Cost *native_startup_costs;
	Cost *native_total_costs;
	bool *replay_applied;
	bool model_applied = false;
	int path_count = list_length(paths);
	int path_mode = plan_replay_effective_channel_mode(plan_replay_path_mode);
	int index = 0;

	if (path_count == 0)
		return;
	native_startup_costs = palloc(sizeof(Cost) * path_count);
	native_total_costs = palloc(sizeof(Cost) * path_count);
	replay_applied = palloc0(sizeof(bool) * path_count);

	plan_replay_batch_count++;
	foreach(cell, paths)
	{
		Path *path = (Path *) lfirst(cell);

		native_startup_costs[index] = path->startup_cost;
		native_total_costs[index] = path->total_cost;

		path->plan_replay_path_sequence_valid = true;
		path->plan_replay_path_sequence = plan_replay_path_sequence;
		{
			PlanReplayPathId id = plan_replay_compute_path_id(path);

			path->plan_replay_path_id_valid = true;
			path->plan_replay_path_id_hi = id.hi;
			path->plan_replay_path_id_lo = id.lo;
		}
		path->plan_replay_cardinality_sequence_valid = false;
		path->plan_replay_cardinality_sequence = 0;
		if (path->param_info != NULL)
		{
			path->plan_replay_cardinality_sequence_valid =
				path->param_info->plan_replay_cardinality_sequence_valid;
			path->plan_replay_cardinality_sequence =
				path->param_info->plan_replay_cardinality_sequence;
		}
		else if (path->parent != NULL)
		{
			path->plan_replay_cardinality_sequence_valid =
				path->parent->plan_replay_cardinality_sequence_valid;
			path->plan_replay_cardinality_sequence =
				path->parent->plan_replay_cardinality_sequence;
		}
		if (plan_replay_cost_table != NULL)
			replay_applied[index] = plan_replay_replay_path(path);
		index++;
	}

	if (plan_replay_mode_predicts(path_mode))
		model_applied = plan_replay_online_predict_paths(plan_replay_planner_root,
												  rel, paths, partial,
												  &plan_replay_current_online_stats);

	index = 0;
	foreach(cell, paths)
	{
		Path *path = (Path *) lfirst(cell);

		if (plan_replay_path_capture_stream != NULL)
			plan_replay_capture_path(rel, path, partial,
								 native_startup_costs[index],
								 native_total_costs[index],
								 replay_applied[index], model_applied);
		plan_replay_path_sequence++;
		index++;
	}
	pfree(replay_applied);
	pfree(native_total_costs);
	pfree(native_startup_costs);
}

static void
plan_replay_require_absolute_file(const char *filename, const char *description)
{
	if (filename == NULL || filename[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay %s file path is not configured", description)));
	if (!is_absolute_path(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay %s file path must be absolute: \"%s\"",
						description, filename)));
}

static void
plan_replay_require_distinct_files(const char *left, const char *left_description,
								   const char *right, const char *right_description)
{
	if (left != NULL && left[0] != '\0' && right != NULL && right[0] != '\0' &&
		strcmp(left, right) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_plan_replay %s and %s files must be different",
						left_description, right_description)));
}

static void
plan_replay_create_path_tables(bool replay_paths)
{
	HASHCTL control;

	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(PlanReplayPathId);
	control.entrysize = sizeof(PlanReplayIdentityEntry);
	plan_replay_identity_table =
		hash_create("pg_plan_replay stable Path descriptors", 1024,
					&control, HASH_ELEM | HASH_BLOBS);
	if (replay_paths)
	{
		MemSet(&control, 0, sizeof(control));
		control.keysize = sizeof(PlanReplayPathId);
		control.entrysize = sizeof(PlanReplayCostEntry);
		plan_replay_cost_table =
			hash_create("pg_plan_replay sparse Path costs", 128,
						&control, HASH_ELEM | HASH_BLOBS);
	}
}

static void
plan_replay_destroy_path_tables(void)
{
	if (plan_replay_identity_table != NULL)
	{
		HASH_SEQ_STATUS status;
		PlanReplayIdentityEntry *entry;

		hash_seq_init(&status, plan_replay_identity_table);
		while ((entry = hash_seq_search(&status)) != NULL)
			pfree(entry->descriptor);
		hash_destroy(plan_replay_identity_table);
		plan_replay_identity_table = NULL;
	}
	if (plan_replay_cost_table != NULL)
	{
		hash_destroy(plan_replay_cost_table);
		plan_replay_cost_table = NULL;
	}
}

static void
plan_replay_begin_statement(void)
{
	int path_mode = plan_replay_effective_channel_mode(plan_replay_path_mode);
	int cardinality_mode =
		plan_replay_effective_channel_mode(plan_replay_cardinality_mode);
	bool capture_paths = plan_replay_mode_captures(path_mode);
	bool replay_paths = plan_replay_mode_replays(path_mode);
	bool predict_paths = plan_replay_mode_predicts(path_mode);
	bool capture_cardinality = plan_replay_mode_captures(cardinality_mode);
	bool replay_cardinality = plan_replay_mode_replays(cardinality_mode);

	plan_replay_statement_active = false;
	if (!capture_paths && !replay_paths && !predict_paths &&
		!capture_cardinality && !replay_cardinality)
		return;
	plan_replay_statement_active = true;

	if (capture_paths)
		plan_replay_require_absolute_file(plan_replay_path_capture_file,
									  "Path capture");
	if (replay_paths)
		plan_replay_require_absolute_file(plan_replay_path_replay_file,
									  "Path replay");
	if (capture_cardinality)
		plan_replay_require_absolute_file(plan_replay_cardinality_capture_file,
									  "cardinality capture");
	if (replay_cardinality)
		plan_replay_require_absolute_file(plan_replay_cardinality_replay_file,
									  "cardinality replay");

	if (capture_paths && replay_paths)
		plan_replay_require_distinct_files(plan_replay_path_capture_file,
									   "Path capture",
									   plan_replay_path_replay_file,
									   "Path replay");
	if (capture_cardinality && replay_cardinality)
		plan_replay_require_distinct_files(plan_replay_cardinality_capture_file,
									   "cardinality capture",
									   plan_replay_cardinality_replay_file,
									   "cardinality replay");
	if (capture_paths && capture_cardinality)
		plan_replay_require_distinct_files(plan_replay_path_capture_file,
									   "Path capture",
									   plan_replay_cardinality_capture_file,
									   "cardinality capture");
	if (replay_paths && replay_cardinality)
		plan_replay_require_distinct_files(plan_replay_path_replay_file,
									   "Path replay",
									   plan_replay_cardinality_replay_file,
									   "cardinality replay");
	if (capture_paths && replay_cardinality)
		plan_replay_require_distinct_files(plan_replay_path_capture_file,
									   "Path capture",
									   plan_replay_cardinality_replay_file,
									   "cardinality replay");
	if (replay_paths && capture_cardinality)
		plan_replay_require_distinct_files(plan_replay_path_replay_file,
									   "Path replay",
									   plan_replay_cardinality_capture_file,
									   "cardinality capture");

	plan_replay_path_sequence = 0;
	plan_replay_batch_count = 0;
	plan_replay_cardinality_sequence = 0;
	plan_replay_cost_replay_entry_count = 0;
	plan_replay_cost_replay_applied_count = 0;
	plan_replay_cost_replay_matched_entry_count = 0;
	plan_replay_online_reset_stats(&plan_replay_current_online_stats);
	plan_replay_planner_root = NULL;
	if (capture_paths || replay_paths || predict_paths)
		plan_replay_create_path_tables(replay_paths);

	if (replay_paths)
	{
		plan_replay_path_replay_stream =
			AllocateFile(plan_replay_path_replay_file, "r");
		if (plan_replay_path_replay_stream == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open pg_plan_replay Path replay file \"%s\": %m",
								plan_replay_path_replay_file)));
		plan_replay_load_costs();
	}
	if (replay_cardinality)
	{
		plan_replay_cardinality_replay_stream =
			AllocateFile(plan_replay_cardinality_replay_file, "r");
		if (plan_replay_cardinality_replay_stream == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay cardinality replay file \"%s\": %m",
							plan_replay_cardinality_replay_file)));
	}
	if (capture_paths)
	{
		plan_replay_path_capture_stream =
			AllocateFile(plan_replay_path_capture_file, "w");
		if (plan_replay_path_capture_stream == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay Path capture file \"%s\": %m",
							plan_replay_path_capture_file)));
	}
	if (capture_cardinality)
	{
		plan_replay_cardinality_capture_stream =
			AllocateFile(plan_replay_cardinality_capture_file, "w");
		if (plan_replay_cardinality_capture_stream == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay cardinality capture file \"%s\": %m",
							plan_replay_cardinality_capture_file)));
	}
}

static void
plan_replay_abort_statement(void)
{
	FILE *path_capture = plan_replay_path_capture_stream;
	FILE *path_replay = plan_replay_path_replay_stream;
	FILE *cardinality_capture = plan_replay_cardinality_capture_stream;
	FILE *cardinality_replay = plan_replay_cardinality_replay_stream;

	plan_replay_path_capture_stream = NULL;
	plan_replay_path_replay_stream = NULL;
	plan_replay_cardinality_capture_stream = NULL;
	plan_replay_cardinality_replay_stream = NULL;
	plan_replay_destroy_path_tables();
	plan_replay_planner_root = NULL;
	plan_replay_statement_active = false;
	if (path_capture != NULL)
		FreeFile(path_capture);
	if (path_replay != NULL)
		FreeFile(path_replay);
	if (cardinality_capture != NULL)
		FreeFile(cardinality_capture);
	if (cardinality_replay != NULL)
		FreeFile(cardinality_replay);
}

static void
plan_replay_finish_file(FILE **file_pointer, bool replay, const char *description,
				 uint64 count, const char *item_name)
{
	FILE *file = *file_pointer;
	int result;

	*file_pointer = NULL;
	if (file == NULL)
		return;

	if (replay)
	{
		int ch;

		while ((ch = fgetc(file)) != EOF)
		{
			if (!isspace((unsigned char) ch))
			{
				FreeFile(file);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("pg_plan_replay %s replay file contains extra values after %s "
								UINT64_FORMAT, description, item_name, count)));
			}
		}
		if (ferror(file))
		{
			FreeFile(file);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not finish reading pg_plan_replay %s replay file: %m",
							description)));
		}
	}
	else if (fflush(file) != 0 || ferror(file))
	{
		FreeFile(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not flush pg_plan_replay %s capture file: %m",
						description)));
	}

	result = FreeFile(file);
	if (result != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pg_plan_replay %s file: %m", description)));
}

static void
plan_replay_finish_statement(void)
{
	uint64 unmatched_costs = plan_replay_cost_replay_entry_count -
		plan_replay_cost_replay_matched_entry_count;

	if (!plan_replay_statement_active)
		return;
	plan_replay_finish_file(&plan_replay_path_replay_stream, true, "Path cost",
					plan_replay_path_sequence, "Path");
	plan_replay_finish_file(&plan_replay_path_capture_stream, false, "Path cost",
					plan_replay_path_sequence, "Path");
	plan_replay_finish_file(&plan_replay_cardinality_replay_stream, true,
						"cardinality", plan_replay_cardinality_sequence, "estimate");
	plan_replay_finish_file(&plan_replay_cardinality_capture_stream, false,
						"cardinality", plan_replay_cardinality_sequence, "estimate");
	plan_replay_planner_root = NULL;
	plan_replay_last_path_count = plan_replay_path_sequence;
	plan_replay_last_batch_count = plan_replay_batch_count;
	plan_replay_last_cost_replay_entry_count =
		plan_replay_cost_replay_entry_count;
	plan_replay_last_cost_replay_applied_count =
		plan_replay_cost_replay_applied_count;
	plan_replay_last_cost_replay_unmatched_count = unmatched_costs;
	plan_replay_last_online_stats = plan_replay_current_online_stats;
	plan_replay_destroy_path_tables();
	plan_replay_statement_active = false;
}

static void
plan_replay_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
					 Index rti, RangeTblEntry *rte)
{
	if (previous_set_rel_pathlist_hook != NULL)
		previous_set_rel_pathlist_hook(root, rel, rti, rte);
	plan_replay_planner_root = root;
	if (plan_replay_is_active())
		flush_path_cost_model(rel);
}

static void
plan_replay_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					  RelOptInfo *outerrel, RelOptInfo *innerrel,
					  JoinType jointype, JoinPathExtraData *extra)
{
	if (previous_set_join_pathlist_hook != NULL)
		previous_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									jointype, extra);
	plan_replay_planner_root = root;
	if (plan_replay_is_active())
		flush_path_cost_model(joinrel);
}

static PlannedStmt *
plan_replay_planner(Query *parse, const char *query_string, int cursor_options,
			ParamListInfo bound_params)
{
	PlannedStmt *result = NULL;
	bool top_level = plan_replay_planner_depth == 0;
	instr_time started;
	instr_time elapsed;

	if (top_level)
		INSTR_TIME_SET_CURRENT(started);
	plan_replay_planner_depth++;
	PG_TRY();
	{
		if (top_level)
			plan_replay_begin_statement();
		if (previous_planner_hook != NULL)
			result = previous_planner_hook(parse, query_string, cursor_options,
									   bound_params);
		else
			result = standard_planner(parse, query_string, cursor_options,
								  bound_params);
		if (top_level)
		{
			bool statement_was_active = plan_replay_statement_active;

			plan_replay_finish_statement();
			if (statement_was_active)
			{
				INSTR_TIME_SET_CURRENT(elapsed);
				INSTR_TIME_SUBTRACT(elapsed, started);
				plan_replay_last_planning_ms = INSTR_TIME_GET_MILLISEC(elapsed);
			}
		}
		plan_replay_planner_depth--;
	}
	PG_CATCH();
	{
		plan_replay_planner_depth--;
		if (top_level)
			plan_replay_abort_statement();
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

Datum
plan_replay_last_timing(PG_FUNCTION_ARGS)
{
	char json[640];

	snprintf(json, sizeof(json),
			 "{\"planning_ms\":%.6f,\"execution_ms\":0,\"inference_ms\":%.6f,"
			 "\"batches\":" UINT64_FORMAT ",\"paths\":" UINT64_FORMAT
			 ",\"model_batches\":" UINT64_FORMAT
			 ",\"model_paths\":" UINT64_FORMAT
			 ",\"model_applied\":" UINT64_FORMAT
			 ",\"cost_replay_entries\":" UINT64_FORMAT
			 ",\"cost_replay_applied\":" UINT64_FORMAT
			 ",\"cost_replay_unmatched\":" UINT64_FORMAT
			 ",\"fallbacks\":0}",
			 plan_replay_last_planning_ms,
			 plan_replay_last_online_stats.inference_ms,
			 plan_replay_last_batch_count, plan_replay_last_path_count,
			 plan_replay_last_online_stats.batches,
			 plan_replay_last_online_stats.paths,
			 plan_replay_last_online_stats.applied,
			 plan_replay_last_cost_replay_entry_count,
			 plan_replay_last_cost_replay_applied_count,
			 plan_replay_last_cost_replay_unmatched_count);
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}

Datum
plan_replay_reset_stats(PG_FUNCTION_ARGS)
{
	plan_replay_path_sequence = 0;
	plan_replay_batch_count = 0;
	plan_replay_last_path_count = 0;
	plan_replay_last_batch_count = 0;
	plan_replay_cardinality_sequence = 0;
	plan_replay_cost_replay_entry_count = 0;
	plan_replay_cost_replay_applied_count = 0;
	plan_replay_cost_replay_matched_entry_count = 0;
	plan_replay_last_cost_replay_entry_count = 0;
	plan_replay_last_cost_replay_applied_count = 0;
	plan_replay_last_cost_replay_unmatched_count = 0;
	plan_replay_last_planning_ms = 0;
	plan_replay_online_reset_stats(&plan_replay_current_online_stats);
	plan_replay_online_reset_stats(&plan_replay_last_online_stats);
	PG_RETURN_VOID();
}

void
_PG_init(void)
{
		DefineCustomEnumVariable("plan_replay.mode",
								 "Offline Path cost and cardinality operation mode.",
						 NULL,
						 &plan_replay_mode,
						 PLAN_REPLAY_MODE_OFF,
						 plan_replay_mode_options,
						 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("plan_replay.path_mode",
						 "Path channel mode; inherit uses plan_replay.mode.",
						 NULL,
						 &plan_replay_path_mode,
						 PLAN_REPLAY_CHANNEL_INHERIT,
						 plan_replay_channel_mode_options,
						 PGC_SUSET,
						 0,
						 NULL, NULL, NULL);
	DefineCustomEnumVariable("plan_replay.cardinality_mode",
						 "Cardinality channel mode; inherit uses plan_replay.mode.",
						 NULL,
						 &plan_replay_cardinality_mode,
						 PLAN_REPLAY_CHANNEL_INHERIT,
						 plan_replay_cardinality_mode_options,
						 PGC_SUSET,
						 0,
						 NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.path_capture_file",
						   "Absolute JSONL output path for one statement.",
						   NULL,
						   &plan_replay_path_capture_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.path_replay_file",
						   "Absolute sparse stable-ID cost input path for one statement.",
						   NULL,
						   &plan_replay_path_replay_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.cardinality_capture_file",
						   "Absolute cardinality JSONL output path for one statement.",
						   NULL,
						   &plan_replay_cardinality_capture_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.cardinality_replay_file",
						   "Absolute one-column cardinality input path for one statement.",
						   NULL,
						   &plan_replay_cardinality_replay_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	plan_replay_online_init();

	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = plan_replay_set_rel_pathlist;
	previous_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = plan_replay_set_join_pathlist;
	previous_planner_hook = planner_hook;
	planner_hook = plan_replay_planner;
	path_cost_model_active_hook = plan_replay_is_active;
	path_cost_model_batch_hook = plan_replay_process_paths;
	previous_cardinality_estimate_hook = cardinality_estimate_hook;
	cardinality_estimate_hook = plan_replay_cardinality_estimate;
}

void
_PG_fini(void)
{
	if (set_rel_pathlist_hook == plan_replay_set_rel_pathlist)
		set_rel_pathlist_hook = previous_set_rel_pathlist_hook;
	if (set_join_pathlist_hook == plan_replay_set_join_pathlist)
		set_join_pathlist_hook = previous_set_join_pathlist_hook;
	if (planner_hook == plan_replay_planner)
		planner_hook = previous_planner_hook;
	if (path_cost_model_active_hook == plan_replay_is_active)
		path_cost_model_active_hook = NULL;
	if (path_cost_model_batch_hook == plan_replay_process_paths)
		path_cost_model_batch_hook = NULL;
	if (cardinality_estimate_hook == plan_replay_cardinality_estimate)
		cardinality_estimate_hook = previous_cardinality_estimate_hook;
	plan_replay_abort_statement();
}
