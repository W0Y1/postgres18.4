#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "commands/dbcommands.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"

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

static int plan_replay_mode = PLAN_REPLAY_MODE_OFF;
static char *plan_replay_path_capture_file = NULL;
static char *plan_replay_path_replay_file = NULL;
static bool plan_replay_path_capture_verbose = false;
static char *plan_replay_cardinality_capture_file = NULL;
static char *plan_replay_cardinality_replay_file = NULL;

static planner_hook_type previous_planner_hook = NULL;
static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static cardinality_estimate_hook_type previous_cardinality_estimate_hook = NULL;

static FILE *plan_replay_path_file = NULL;
static FILE *plan_replay_cardinality_file = NULL;
static PlannerInfo *plan_replay_planner_root = NULL;
static Query *plan_replay_query = NULL;
static const char *plan_replay_query_string = NULL;
static int plan_replay_cursor_options = 0;
static uint64 plan_replay_path_sequence = 0;
static uint64 plan_replay_batch_count = 0;
static uint64 plan_replay_last_path_count = 0;
static uint64 plan_replay_last_batch_count = 0;
static uint64 plan_replay_cardinality_sequence = 0;
static int plan_replay_planner_depth = 0;

static const struct config_enum_entry plan_replay_mode_options[] = {
	{"off", PLAN_REPLAY_MODE_OFF, false},
	{"capture", PLAN_REPLAY_MODE_CAPTURE, false},
	{"replay", PLAN_REPLAY_MODE_REPLAY, false},
	{NULL, 0, false}
};

static const char *plan_replay_operator_name(Path *path);
static const char *plan_replay_path_node_name(Path *path);
static const char *plan_replay_join_type_name(JoinType jointype);
static void plan_replay_append_path_json(StringInfo output, Path *path);
static void plan_replay_append_compact_path_json(StringInfo output, Path *path);
static void plan_replay_append_verbose_path_json(StringInfo output, Path *path);
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

static const char *
plan_replay_reloptkind_name(RelOptKind kind)
{
	switch (kind)
	{
		case RELOPT_BASEREL: return "baserel";
		case RELOPT_JOINREL: return "joinrel";
		case RELOPT_OTHER_MEMBER_REL: return "other_member_rel";
		case RELOPT_OTHER_JOINREL: return "other_joinrel";
		case RELOPT_UPPER_REL: return "upper_rel";
		case RELOPT_OTHER_UPPER_REL: return "other_upper_rel";
	}
	return "unknown";
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
plan_replay_append_relation_names(StringInfo output, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while (plan_replay_planner_root != NULL &&
		   (member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(member, plan_replay_planner_root);
		char *name;

		if (rte == NULL || rte->rtekind != RTE_RELATION)
			continue;
		name = get_rel_name(rte->relid);
		if (name == NULL)
			continue;
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		escape_json(output, name);
		pfree(name);
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
plan_replay_append_relation_columns(StringInfo output, RangeTblEntry *rte)
{
	ListCell *cell;
	AttrNumber attnum = 1;
	bool first = true;

	appendStringInfoChar(output, '[');
	foreach(cell, rte->eref->colnames)
	{
		const char *alias = strVal(lfirst(cell));
		char *name = get_attname(rte->relid, attnum, true);
		Oid type_oid = get_atttype(rte->relid, attnum);
		char *type_name = OidIsValid(type_oid) ? format_type_be(type_oid) : NULL;

		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "{\"attnum\":%d,\"name\":", attnum);
		if (name != NULL)
			escape_json(output, name);
		else
			appendStringInfoString(output, "null");
		appendStringInfoString(output, ",\"alias\":");
		escape_json(output, alias);
		appendStringInfo(output, ",\"type_oid\":%u,\"type_name\":", type_oid);
		if (type_name != NULL)
			escape_json(output, type_name);
		else
			appendStringInfoString(output, "null");
		appendStringInfoChar(output, '}');
		if (name != NULL)
			pfree(name);
		if (type_name != NULL)
			pfree(type_name);
		attnum++;
	}
	appendStringInfoChar(output, ']');
}

static void
plan_replay_append_relations(StringInfo output, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while (plan_replay_planner_root != NULL &&
		   (member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(member, plan_replay_planner_root);

		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "{\"rti\":%d,\"rtekind\":%d", member,
						 rte != NULL ? (int) rte->rtekind : -1);
		if (rte != NULL && rte->eref != NULL)
		{
			appendStringInfoString(output, ",\"alias\":");
			escape_json(output, rte->eref->aliasname);
		}
		if (rte != NULL && rte->rtekind == RTE_RELATION)
		{
			char *name = get_rel_name(rte->relid);
			char *schema = get_namespace_name(get_rel_namespace(rte->relid));

			appendStringInfo(output, ",\"relation_oid\":%u,\"schema\":",
							 rte->relid);
			if (schema != NULL)
				escape_json(output, schema);
			else
				appendStringInfoString(output, "null");
			appendStringInfoString(output, ",\"name\":");
			if (name != NULL)
				escape_json(output, name);
			else
				appendStringInfoString(output, "null");
			appendStringInfoString(output, ",\"columns\":");
			plan_replay_append_relation_columns(output, rte);
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
plan_replay_append_planner_settings(StringInfo output)
{
	static const char *const names[] = {
		"search_path",
		"plan_cache_mode",
		"default_statistics_target",
		"effective_cache_size",
		"effective_io_concurrency",
		"work_mem",
		"seq_page_cost",
		"random_page_cost",
		"cpu_tuple_cost",
		"cpu_index_tuple_cost",
		"cpu_operator_cost",
		"parallel_setup_cost",
		"parallel_tuple_cost",
		"min_parallel_table_scan_size",
		"min_parallel_index_scan_size",
		"max_parallel_workers_per_gather",
		"cursor_tuple_fraction",
		"from_collapse_limit",
		"join_collapse_limit",
		"geqo",
		"geqo_threshold",
		"geqo_seed",
		"enable_async_append",
		"enable_bitmapscan",
		"enable_gathermerge",
		"enable_hashagg",
		"enable_hashjoin",
		"enable_incremental_sort",
		"enable_indexscan",
		"enable_indexonlyscan",
		"enable_material",
		"enable_memoize",
		"enable_mergejoin",
		"enable_nestloop",
		"enable_parallel_append",
		"enable_parallel_hash",
		"enable_partition_pruning",
		"enable_partitionwise_aggregate",
		"enable_partitionwise_join",
		"enable_presorted_aggregate",
		"enable_seqscan",
		"enable_sort",
		"enable_tidscan"
	};
	bool first = true;
	int i;

	appendStringInfoChar(output, '{');
	for (i = 0; i < lengthof(names); i++)
	{
		char *value = GetConfigOptionByName(names[i], NULL, true);

		if (value == NULL)
			continue;
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		escape_json(output, names[i]);
		appendStringInfoChar(output, ':');
		escape_json(output, value);
		pfree(value);
	}
	appendStringInfoChar(output, '}');
}

static void
plan_replay_append_pathtarget(StringInfo output, PathTarget *target)
{
	ListCell *cell;
	int index = 0;
	bool first = true;

	if (target == NULL)
	{
		appendStringInfoString(output, "null");
		return;
	}
	appendStringInfo(output,
					 "{\"width\":%d,\"startup_cost\":%.17g,"
					 "\"per_tuple_cost\":%.17g,\"has_volatile_expr\":%d,"
					 "\"expressions\":[",
					 target->width, target->cost.startup, target->cost.per_tuple,
					 (int) target->has_volatile_expr);
	foreach(cell, target->exprs)
	{
		Node *expr = (Node *) lfirst(cell);
		Index sortgroupref = get_pathtarget_sortgroupref(target, index++);

		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "{\"sortgroupref\":%u,\"expression\":",
						 sortgroupref);
		plan_replay_append_node_dump(output, expr);
		appendStringInfoChar(output, '}');
	}
	appendStringInfoString(output, "]}");
}

static void
plan_replay_append_relation_info(StringInfo output, RelOptInfo *rel)
{
	appendStringInfoString(output, "{\"kind\":");
	escape_json(output, plan_replay_reloptkind_name(rel->reloptkind));
	appendStringInfo(output,
					 ",\"relid\":%u,\"rtekind\":%d,\"rows\":%.17g,"
					 "\"pages\":%u,\"tuples\":%.17g,\"allvisfrac\":%.17g,"
					 "\"consider_startup\":%s,\"consider_param_startup\":%s,"
					 "\"consider_parallel\":%s,\"rel_parallel_workers\":%d,"
					 "\"baserestrict_startup_cost\":%.17g,"
					 "\"baserestrict_per_tuple_cost\":%.17g,\"relids\":",
					 rel->relid, (int) rel->rtekind, rel->rows, rel->pages,
					 rel->tuples, rel->allvisfrac,
					 rel->consider_startup ? "true" : "false",
					 rel->consider_param_startup ? "true" : "false",
					 rel->consider_parallel ? "true" : "false",
					 rel->rel_parallel_workers,
					 rel->baserestrictcost.startup,
					 rel->baserestrictcost.per_tuple);
	plan_replay_append_relids(output, rel->relids);
	appendStringInfoString(output, ",\"direct_lateral_relids\":");
	plan_replay_append_relids(output, rel->direct_lateral_relids);
	appendStringInfoString(output, ",\"lateral_relids\":");
	plan_replay_append_relids(output, rel->lateral_relids);
	appendStringInfoString(output, ",\"base_restrictions\":");
	plan_replay_append_node_dump(output, rel->baserestrictinfo);
	appendStringInfoString(output, ",\"join_clauses\":");
	plan_replay_append_node_dump(output, rel->joininfo);
	appendStringInfoString(output, ",\"partition_qual\":");
	plan_replay_append_node_dump(output, rel->partition_qual);
	appendStringInfoChar(output, '}');
}

static void
plan_replay_append_path_details(StringInfo output, Path *path)
{
	appendStringInfoChar(output, '{');
	switch (nodeTag(path))
	{
		case T_IndexPath:
		{
			IndexPath *index_path = (IndexPath *) path;
			IndexOptInfo *index = index_path->indexinfo;

			appendStringInfo(output,
							 "\"scan_direction\":%d,\"index_total_cost\":%.17g,"
							 "\"index_selectivity\":%.17g,\"index_clauses\":",
							 (int) index_path->indexscandir,
							 index_path->indextotalcost,
							 index_path->indexselectivity);
			plan_replay_append_node_dump(output, index_path->indexclauses);
			appendStringInfoString(output, ",\"index_orderbys\":");
			plan_replay_append_node_dump(output, index_path->indexorderbys);
			appendStringInfoString(output, ",\"index_orderby_columns\":");
			plan_replay_append_node_dump(output, index_path->indexorderbycols);
			if (index != NULL)
			{
				appendStringInfo(output,
								 ",\"index_oid\":%u,\"index_pages\":%u,"
								 "\"index_tuples\":%.17g,\"tree_height\":%d,"
								 "\"ncolumns\":%d,\"nkeycolumns\":%d,"
								 "\"unique\":%s,\"predicate_matches\":%s,"
								 "\"index_predicate\":",
								 index->indexoid, index->pages, index->tuples,
								 index->tree_height, index->ncolumns,
								 index->nkeycolumns,
								 index->unique ? "true" : "false",
								 index->predOK ? "true" : "false");
				plan_replay_append_node_dump(output, index->indpred);
				appendStringInfoString(output, ",\"index_expressions\":");
				plan_replay_append_node_dump(output, index->indexprs);
				appendStringInfoString(output, ",\"index_restrictions\":");
				plan_replay_append_node_dump(output, index->indrestrictinfo);
			}
			break;
		}
		case T_BitmapAndPath:
			appendStringInfo(output, "\"bitmap_selectivity\":%.17g",
							 ((BitmapAndPath *) path)->bitmapselectivity);
			break;
		case T_BitmapOrPath:
			appendStringInfo(output, "\"bitmap_selectivity\":%.17g",
							 ((BitmapOrPath *) path)->bitmapselectivity);
			break;
		case T_TidPath:
			appendStringInfoString(output, "\"tid_quals\":");
			plan_replay_append_node_dump(output, ((TidPath *) path)->tidquals);
			break;
		case T_TidRangePath:
			appendStringInfoString(output, "\"tid_range_quals\":");
			plan_replay_append_node_dump(output, ((TidRangePath *) path)->tidrangequals);
			break;
		case T_ForeignPath:
			appendStringInfoString(output, "\"fdw_restrictions\":");
			plan_replay_append_node_dump(output, ((ForeignPath *) path)->fdw_restrictinfo);
			appendStringInfoString(output, ",\"fdw_private\":");
			plan_replay_append_node_dump(output, ((ForeignPath *) path)->fdw_private);
			break;
		case T_CustomPath:
			appendStringInfo(output, "\"flags\":%u,\"custom_restrictions\":",
							 ((CustomPath *) path)->flags);
			plan_replay_append_node_dump(output, ((CustomPath *) path)->custom_restrictinfo);
			appendStringInfoString(output, ",\"custom_private\":");
			plan_replay_append_node_dump(output, ((CustomPath *) path)->custom_private);
			break;
		case T_AppendPath:
			appendStringInfo(output,
							 "\"first_partial_path\":%d,\"limit_tuples\":%.17g",
							 ((AppendPath *) path)->first_partial_path,
							 ((AppendPath *) path)->limit_tuples);
			break;
		case T_MergeAppendPath:
			appendStringInfo(output, "\"limit_tuples\":%.17g",
							 ((MergeAppendPath *) path)->limit_tuples);
			break;
		case T_GroupResultPath:
			appendStringInfoString(output, "\"quals\":");
			plan_replay_append_node_dump(output, ((GroupResultPath *) path)->quals);
			break;
		case T_MemoizePath:
		{
			MemoizePath *memoize = (MemoizePath *) path;

			appendStringInfo(output,
							 "\"single_row\":%s,\"binary_mode\":%s,"
							 "\"calls\":%.17g,\"estimated_entries\":%u,"
							 "\"hash_operators\":",
							 memoize->singlerow ? "true" : "false",
							 memoize->binary_mode ? "true" : "false",
							 memoize->calls, memoize->est_entries);
			plan_replay_append_node_dump(output, memoize->hash_operators);
			appendStringInfoString(output, ",\"parameter_expressions\":");
			plan_replay_append_node_dump(output, memoize->param_exprs);
			break;
		}
		case T_UniquePath:
			appendStringInfo(output, "\"method\":%d,\"operators\":",
							 (int) ((UniquePath *) path)->umethod);
			plan_replay_append_node_dump(output, ((UniquePath *) path)->in_operators);
			appendStringInfoString(output, ",\"expressions\":");
			plan_replay_append_node_dump(output, ((UniquePath *) path)->uniq_exprs);
			break;
		case T_GatherPath:
			appendStringInfo(output, "\"single_copy\":%s,\"workers\":%d",
							 ((GatherPath *) path)->single_copy ? "true" : "false",
							 ((GatherPath *) path)->num_workers);
			break;
		case T_GatherMergePath:
			appendStringInfo(output, "\"workers\":%d",
							 ((GatherMergePath *) path)->num_workers);
			break;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
		{
			JoinPath *join = (JoinPath *) path;

			appendStringInfo(output,
							 "\"join_type\":%d,\"inner_unique\":%s,"
							 "\"join_restrictions\":",
							 (int) join->jointype,
							 join->inner_unique ? "true" : "false");
			plan_replay_append_node_dump(output, join->joinrestrictinfo);
			if (IsA(path, MergePath))
			{
				MergePath *merge = (MergePath *) path;

				appendStringInfoString(output, ",\"merge_clauses\":");
				plan_replay_append_node_dump(output, merge->path_mergeclauses);
				appendStringInfoString(output, ",\"outer_sortkeys\":");
				plan_replay_append_node_dump(output, merge->outersortkeys);
				appendStringInfoString(output, ",\"inner_sortkeys\":");
				plan_replay_append_node_dump(output, merge->innersortkeys);
				appendStringInfo(output,
								 ",\"outer_presorted_keys\":%d,"
								 "\"skip_mark_restore\":%s,"
								 "\"materialize_inner\":%s",
								 merge->outer_presorted_keys,
								 merge->skip_mark_restore ? "true" : "false",
								 merge->materialize_inner ? "true" : "false");
			}
			else if (IsA(path, HashPath))
			{
				HashPath *hash = (HashPath *) path;

				appendStringInfoString(output, ",\"hash_clauses\":");
				plan_replay_append_node_dump(output, hash->path_hashclauses);
				appendStringInfo(output,
								 ",\"batches\":%d,\"inner_rows_total\":%.17g",
								 hash->num_batches, hash->inner_rows_total);
			}
			break;
		}
		case T_ProjectionPath:
			appendStringInfo(output, "\"dummy\":%s",
							 ((ProjectionPath *) path)->dummypp ? "true" : "false");
			break;
		case T_IncrementalSortPath:
			appendStringInfo(output, "\"presorted_columns\":%d",
							 ((IncrementalSortPath *) path)->nPresortedCols);
			break;
		case T_GroupPath:
			appendStringInfoString(output, "\"group_clause\":");
			plan_replay_append_node_dump(output, ((GroupPath *) path)->groupClause);
			appendStringInfoString(output, ",\"having_quals\":");
			plan_replay_append_node_dump(output, ((GroupPath *) path)->qual);
			break;
		case T_UpperUniquePath:
			appendStringInfo(output, "\"keys\":%d",
							 ((UpperUniquePath *) path)->numkeys);
			break;
		case T_AggPath:
		{
			AggPath *agg = (AggPath *) path;

			appendStringInfo(output,
							 "\"strategy\":%d,\"split\":%d,\"groups\":%.17g,"
							 "\"transition_space\":" UINT64_FORMAT ","
							 "\"group_clause\":",
							 (int) agg->aggstrategy, (int) agg->aggsplit,
							 agg->numGroups, agg->transitionSpace);
			plan_replay_append_node_dump(output, agg->groupClause);
			appendStringInfoString(output, ",\"having_quals\":");
			plan_replay_append_node_dump(output, agg->qual);
			break;
		}
		case T_GroupingSetsPath:
			appendStringInfo(output, "\"strategy\":%d,\"rollups\":",
							 (int) ((GroupingSetsPath *) path)->aggstrategy);
			plan_replay_append_node_dump(output, ((GroupingSetsPath *) path)->rollups);
			appendStringInfoString(output, ",\"having_quals\":");
			plan_replay_append_node_dump(output, ((GroupingSetsPath *) path)->qual);
			break;
		case T_MinMaxAggPath:
			appendStringInfoString(output, "\"aggregates\":");
			plan_replay_append_node_dump(output, ((MinMaxAggPath *) path)->mmaggregates);
			appendStringInfoString(output, ",\"having_quals\":");
			plan_replay_append_node_dump(output, ((MinMaxAggPath *) path)->quals);
			break;
		case T_WindowAggPath:
			appendStringInfo(output, "\"top_window\":%s,\"window_clause\":",
							 ((WindowAggPath *) path)->topwindow ? "true" : "false");
			plan_replay_append_node_dump(output, ((WindowAggPath *) path)->winclause);
			appendStringInfoString(output, ",\"quals\":");
			plan_replay_append_node_dump(output, ((WindowAggPath *) path)->qual);
			appendStringInfoString(output, ",\"run_condition\":");
			plan_replay_append_node_dump(output, ((WindowAggPath *) path)->runCondition);
			break;
		case T_SetOpPath:
			appendStringInfo(output,
							 "\"command\":%d,\"strategy\":%d,\"groups\":%.17g,"
							 "\"group_list\":",
							 (int) ((SetOpPath *) path)->cmd,
							 (int) ((SetOpPath *) path)->strategy,
							 ((SetOpPath *) path)->numGroups);
			plan_replay_append_node_dump(output, ((SetOpPath *) path)->groupList);
			break;
		case T_RecursiveUnionPath:
			appendStringInfo(output, "\"worktable_param\":%d,\"groups\":%.17g,"
							 "\"distinct_list\":",
							 ((RecursiveUnionPath *) path)->wtParam,
							 ((RecursiveUnionPath *) path)->numGroups);
			plan_replay_append_node_dump(output, ((RecursiveUnionPath *) path)->distinctList);
			break;
		case T_LockRowsPath:
			appendStringInfo(output, "\"epq_param\":%d,\"row_marks\":",
							 ((LockRowsPath *) path)->epqParam);
			plan_replay_append_node_dump(output, ((LockRowsPath *) path)->rowMarks);
			break;
		case T_ModifyTablePath:
			appendStringInfo(output,
							 "\"operation\":%d,\"nominal_relation\":%u,"
							 "\"root_relation\":%u,\"epq_param\":%d,"
							 "\"returning_lists\":",
							 (int) ((ModifyTablePath *) path)->operation,
							 ((ModifyTablePath *) path)->nominalRelation,
							 ((ModifyTablePath *) path)->rootRelation,
							 ((ModifyTablePath *) path)->epqParam);
			plan_replay_append_node_dump(output, ((ModifyTablePath *) path)->returningLists);
			appendStringInfoString(output, ",\"on_conflict\":");
			plan_replay_append_node_dump(output, ((ModifyTablePath *) path)->onconflict);
			appendStringInfoString(output, ",\"merge_join_conditions\":");
			plan_replay_append_node_dump(output,
							 ((ModifyTablePath *) path)->mergeJoinConditions);
			break;
		case T_LimitPath:
			appendStringInfo(output, "\"option\":%d,\"offset\":",
							 (int) ((LimitPath *) path)->limitOption);
			plan_replay_append_node_dump(output, ((LimitPath *) path)->limitOffset);
			appendStringInfoString(output, ",\"count\":");
			plan_replay_append_node_dump(output, ((LimitPath *) path)->limitCount);
			break;
		default:
			break;
	}
	appendStringInfoChar(output, '}');
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
plan_replay_append_verbose_path_json(StringInfo output, Path *path)
{
	bool first_child = true;
	Relids required_outer = PATH_REQ_OUTER(path);

	appendStringInfoChar(output, '{');
	appendStringInfoString(output, "\"node_tag\":");
	escape_json(output, plan_replay_path_node_name(path));
	appendStringInfo(output, ",\"node_tag_id\":%d,\"pathtype_id\":%d,\"op_name\":",
					 (int) nodeTag(path), (int) path->pathtype);
	escape_json(output, plan_replay_operator_name(path));
	appendStringInfo(output,
					 ",\"native_startup_cost\":%.17g"
					 ",\"native_total_cost\":%.17g"
					 ",\"rows\":%.17g,\"width\":%d"
					 ",\"disabled_nodes\":%d"
					 ",\"parallel_aware\":%s,\"parallel_safe\":%s"
					 ",\"parallel_workers\":%d,\"parameterized\":%s,"
					 "\"parameterized_rows\":%.17g,\"required_outer\":",
					 path->startup_cost, path->total_cost, path->rows,
					 path->pathtarget ? path->pathtarget->width : 0,
					 path->disabled_nodes,
					 path->parallel_aware ? "true" : "false",
					 path->parallel_safe ? "true" : "false",
					 path->parallel_workers,
					 path->param_info != NULL ? "true" : "false",
					 path->param_info != NULL ? path->param_info->ppi_rows : path->rows);
	plan_replay_append_relids(output, required_outer);
	appendStringInfoString(output, ",\"parameter_clauses\":");
	plan_replay_append_node_dump(output,
					 path->param_info != NULL ? path->param_info->ppi_clauses : NULL);
	appendStringInfoString(output, ",\"pathkeys\":");
	plan_replay_append_node_dump(output, path->pathkeys);
	appendStringInfo(output, ",\"pathkey_count\":%d,\"pathtarget\":",
					 list_length(path->pathkeys));
	plan_replay_append_pathtarget(output, path->pathtarget);
	appendStringInfoString(output, ",\"relids\":");
	plan_replay_append_relids(output, path->parent ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relation_names\":");
	plan_replay_append_relation_names(output, path->parent ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relations\":");
	plan_replay_append_relations(output, path->parent ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relation_info\":");
	if (path->parent != NULL)
		plan_replay_append_relation_info(output, path->parent);
	else
		appendStringInfoString(output, "null");

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

	appendStringInfoString(output, ",\"raw_path\":");
	plan_replay_append_node_dump(output, path);
	appendStringInfoString(output, ",\"details\":");
	plan_replay_append_path_details(output, path);

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
plan_replay_append_compact_path_json(StringInfo output, Path *path)
{
	bool first_child = true;

	appendStringInfoChar(output, '{');
	appendStringInfoString(output, "\"op_name\":");
	escape_json(output, plan_replay_operator_name(path));
	appendStringInfo(output,
					 ",\"startup_cost\":%.17g,\"total_cost\":%.17g"
					 ",\"rows\":%.17g,\"width\":%d,\"pathkeys\":%d"
					 ",\"parallel_aware\":%s,\"parallel_workers\":%d"
					 ",\"relids\":",
					 path->startup_cost, path->total_cost, path->rows,
					 path->pathtarget != NULL ? path->pathtarget->width : 0,
					 list_length(path->pathkeys),
					 path->parallel_aware ? "true" : "false",
					 path->parallel_workers);
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
	if (plan_replay_path_capture_verbose)
		plan_replay_append_verbose_path_json(output, path);
	else
		plan_replay_append_compact_path_json(output, path);
}

static void
plan_replay_capture_path(RelOptInfo *rel, Path *path, bool partial)
{
	StringInfoData line;
	size_t written;
	char *database_name = NULL;
	char *user_name = NULL;

	initStringInfo(&line);
	if (!plan_replay_path_capture_verbose)
		appendStringInfo(&line,
						 "{\"format_version\":3,\"sequence\":" UINT64_FORMAT,
						 plan_replay_path_sequence);
	else
	{
		database_name = get_database_name(MyDatabaseId);
		user_name = GetUserNameFromId(GetUserId(), false);
		appendStringInfo(&line, "{\"sequence\":" UINT64_FORMAT
						 ",\"format_version\":2,\"server_version_num\":%d,"
						 "\"query_id\":" INT64_FORMAT ",\"command_type\":%d,"
						 "\"cursor_options\":%d,\"database\":",
						 plan_replay_path_sequence, PG_VERSION_NUM,
						 plan_replay_query != NULL ? plan_replay_query->queryId : INT64CONST(0),
						 plan_replay_query != NULL ? (int) plan_replay_query->commandType : 0,
						 plan_replay_cursor_options);
		if (database_name != NULL)
			escape_json(&line, database_name);
		else
			appendStringInfoString(&line, "null");
		appendStringInfoString(&line, ",\"user\":");
		if (user_name != NULL)
			escape_json(&line, user_name);
		else
			appendStringInfoString(&line, "null");
		appendStringInfoString(&line, ",\"query\":");
		if (plan_replay_query_string != NULL)
			escape_json(&line, plan_replay_query_string);
		else
			appendStringInfoString(&line, "null");
		appendStringInfoString(&line, ",\"raw_query\":");
		plan_replay_append_node_dump(&line, plan_replay_query);
		appendStringInfoString(&line, ",\"planner_settings\":");
		plan_replay_append_planner_settings(&line);
	}
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

	written = fwrite(line.data, 1, line.len, plan_replay_path_file);
	if (written != (size_t) line.len)
		ereport(ERROR,
				(errcode_for_file_access(),
					 errmsg("could not write pg_plan_replay capture file: %m")));
	if (database_name != NULL)
		pfree(database_name);
	if (user_name != NULL)
		pfree(user_name);
	pfree(line.data);
}

static void
plan_replay_replay_path(Path *path)
{
	char line[256];
	char *cursor;
	char *end;
	double startup_cost;
	double total_cost;

	if (fgets(line, sizeof(line), plan_replay_path_file) == NULL)
	{
		if (ferror(plan_replay_path_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read pg_plan_replay replay file: %m")));
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay replay file ended before Path " UINT64_FORMAT,
						plan_replay_path_sequence),
				 errdetail("Expected costs for operator %s.", plan_replay_operator_name(path))));
	}
	if (strchr(line, '\n') == NULL && !feof(plan_replay_path_file))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay replay line " UINT64_FORMAT " is too long",
						plan_replay_path_sequence)));

	cursor = line;
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
				 errmsg("invalid pg_plan_replay costs at Path " UINT64_FORMAT,
						plan_replay_path_sequence),
				 errdetail("Operator %s requires finite nonnegative costs with startup_cost <= total_cost.",
						   plan_replay_operator_name(path))));

	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
	return;

invalid_line:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg("malformed pg_plan_replay replay line for Path " UINT64_FORMAT,
					plan_replay_path_sequence),
				 errdetail("Expected exactly: startup_cost total_cost")));
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
						Cardinality inner_rows, Cardinality native_rows)
{
	StringInfoData line;
	size_t written;

	initStringInfo(&line);
	appendStringInfo(&line,
					 "{\"format_version\":1,\"sequence\":" UINT64_FORMAT
					 ",\"kind\":",
					 plan_replay_cardinality_sequence);
	escape_json(&line, plan_replay_cardinality_kind_name(kind));
	appendStringInfo(&line,
					 ",\"native_rows\":%.17g,\"width\":%d,\"relids\":",
					 native_rows, rel->reltarget != NULL ? rel->reltarget->width : 0);
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

	written = fwrite(line.data, 1, line.len, plan_replay_cardinality_file);
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

	if (fgets(line, sizeof(line), plan_replay_cardinality_file) == NULL)
	{
		if (ferror(plan_replay_cardinality_file))
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
	if (strchr(line, '\n') == NULL && !feof(plan_replay_cardinality_file))
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

	if (previous_cardinality_estimate_hook != NULL)
		rows = previous_cardinality_estimate_hook(root, kind, rel,
													  outer_rel, inner_rel, sjinfo,
													  restrictlist, outer_rows,
													  inner_rows, native_rows,
													  sequence_valid, sequence);
	if (plan_replay_mode == PLAN_REPLAY_MODE_OFF ||
		plan_replay_cardinality_file == NULL)
		return rows;

	plan_replay_planner_root = root;
	if (plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE)
		plan_replay_capture_cardinality(root, kind, rel, outer_rel, inner_rel, sjinfo,
									restrictlist, outer_rows, inner_rows, rows);
	else if (plan_replay_mode == PLAN_REPLAY_MODE_REPLAY)
		rows = plan_replay_replay_cardinality(kind);
	else
		elog(ERROR, "pg_plan_replay cardinality hook called with invalid mode");
	*sequence_valid = true;
	*sequence = plan_replay_cardinality_sequence;
	plan_replay_cardinality_sequence++;
	return rows;
}

static bool
plan_replay_is_active(void)
{
	return plan_replay_mode != PLAN_REPLAY_MODE_OFF && plan_replay_path_file != NULL;
}

static void
plan_replay_process_paths(RelOptInfo *rel, List *paths, bool partial)
{
	ListCell *cell;

	plan_replay_batch_count++;
	foreach(cell, paths)
	{
		Path *path = (Path *) lfirst(cell);

		path->plan_replay_path_sequence_valid = true;
		path->plan_replay_path_sequence = plan_replay_path_sequence;
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
		if (plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE)
			plan_replay_capture_path(rel, path, partial);
		else if (plan_replay_mode == PLAN_REPLAY_MODE_REPLAY)
			plan_replay_replay_path(path);
		else
			elog(ERROR, "pg_plan_replay Path hook called while mode is off");
		plan_replay_path_sequence++;
	}
}

static const char *
plan_replay_selected_path_file(void)
{
	return plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE ?
		plan_replay_path_capture_file : plan_replay_path_replay_file;
}

static const char *
plan_replay_selected_cardinality_file(void)
{
	return plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE ?
		plan_replay_cardinality_capture_file : plan_replay_cardinality_replay_file;
}

static void
plan_replay_begin_statement(Query *parse, const char *query_string, int cursor_options)
{
	const char *filename = NULL;
	const char *cardinality_filename = NULL;

	if (plan_replay_mode == PLAN_REPLAY_MODE_OFF)
		return;
	filename = plan_replay_selected_path_file();
	if (filename == NULL || filename[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay Path cost file path is not configured")));
	if (!is_absolute_path(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay Path cost file path must be absolute: \"%s\"",
						filename)));

	cardinality_filename = plan_replay_selected_cardinality_file();
	if (cardinality_filename == NULL || cardinality_filename[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay cardinality file path is not configured")));
	if (!is_absolute_path(cardinality_filename))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay cardinality file path must be absolute: \"%s\"",
						cardinality_filename)));
	if (filename != NULL && cardinality_filename != NULL &&
		strcmp(filename, cardinality_filename) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay Path cost and cardinality files must be different")));

	plan_replay_path_sequence = 0;
	plan_replay_batch_count = 0;
	plan_replay_cardinality_sequence = 0;
	plan_replay_planner_root = NULL;
	plan_replay_query = parse;
	plan_replay_query_string = query_string;
	plan_replay_cursor_options = cursor_options;

	if (filename != NULL)
	{
		plan_replay_path_file = AllocateFile(filename,
								plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE ? "w" : "r");
		if (plan_replay_path_file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay Path cost file \"%s\": %m",
							filename)));
	}
	if (cardinality_filename != NULL)
	{
		plan_replay_cardinality_file = AllocateFile(cardinality_filename,
												plan_replay_mode == PLAN_REPLAY_MODE_CAPTURE ?
												"w" : "r");
		if (plan_replay_cardinality_file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay cardinality file \"%s\": %m",
							cardinality_filename)));
	}
}

static void
plan_replay_abort_statement(void)
{
	FILE *file = plan_replay_path_file;
	FILE *cardinality_file = plan_replay_cardinality_file;

	plan_replay_path_file = NULL;
	plan_replay_cardinality_file = NULL;
	plan_replay_planner_root = NULL;
	plan_replay_query = NULL;
	plan_replay_query_string = NULL;
	plan_replay_cursor_options = 0;
	if (file != NULL)
		FreeFile(file);
	if (cardinality_file != NULL)
		FreeFile(cardinality_file);
}

static void
plan_replay_finish_file(FILE **file_pointer, int mode, const char *description,
				 uint64 count, const char *item_name)
{
	FILE *file = *file_pointer;
	int result;

	*file_pointer = NULL;
	if (file == NULL)
		return;

	if (mode == PLAN_REPLAY_MODE_REPLAY)
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
	plan_replay_finish_file(&plan_replay_path_file, plan_replay_mode, "Path cost",
					plan_replay_path_sequence, "Path");
	plan_replay_finish_file(&plan_replay_cardinality_file, plan_replay_mode,
						"cardinality", plan_replay_cardinality_sequence, "estimate");
	plan_replay_planner_root = NULL;
	plan_replay_last_path_count = plan_replay_path_sequence;
	plan_replay_last_batch_count = plan_replay_batch_count;
	plan_replay_query = NULL;
	plan_replay_query_string = NULL;
	plan_replay_cursor_options = 0;
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

	plan_replay_planner_depth++;
	PG_TRY();
	{
		if (top_level)
			plan_replay_begin_statement(parse, query_string, cursor_options);
		if (previous_planner_hook != NULL)
			result = previous_planner_hook(parse, query_string, cursor_options,
									   bound_params);
		else
			result = standard_planner(parse, query_string, cursor_options,
								  bound_params);
		if (top_level)
			plan_replay_finish_statement();
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
	char json[256];

	snprintf(json, sizeof(json),
			 "{\"planning_ms\":0,\"execution_ms\":0,\"inference_ms\":0,"
			 "\"batches\":" UINT64_FORMAT ",\"paths\":" UINT64_FORMAT
			 ",\"fallbacks\":0}",
			 plan_replay_last_batch_count, plan_replay_last_path_count);
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
	DefineCustomStringVariable("plan_replay.path_capture_file",
						   "Absolute JSONL output path for one statement.",
						   NULL,
						   &plan_replay_path_capture_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.path_replay_file",
						   "Absolute two-column cost input path for one statement.",
						   NULL,
						   &plan_replay_path_replay_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomBoolVariable("plan_replay.path_capture_verbose",
						 "Write the legacy verbose Path JSONL format.",
						 NULL,
						 &plan_replay_path_capture_verbose,
						 false,
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
