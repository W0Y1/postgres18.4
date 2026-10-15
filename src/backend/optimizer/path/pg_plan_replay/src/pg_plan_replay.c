/* pg_plan_replay 扩展入口：GUC、planner 生命周期以及离线采集/回放通道。 */
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
#include "optimizer/optimizer.h"
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
#include "cardinality_identity.h"

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
	PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT,
	PLAN_REPLAY_CHANNEL_PRUNE,
	PLAN_REPLAY_CHANNEL_CAPTURE_PRUNE
} PlanReplayChannelMode;

#define PLAN_REPLAY_PATH_ID_BYTES 16
#define PLAN_REPLAY_PATH_ID_HEX_LENGTH (PLAN_REPLAY_PATH_ID_BYTES * 2)
#define PLAN_REPLAY_COST_LINE_MAX 256
#define PLAN_REPLAY_CARDINALITY_LINE_MAX 256
#define PLAN_REPLAY_PRUNE_LINE_MAX 128

typedef struct PlanReplayPathId
{
	uint64		hi;
	uint64		lo;
} PlanReplayPathId;

typedef struct PlanReplayIdentityEntry
{
	PlanReplayPathId id;
	char	   *descriptor;
	bool		capture_written;
	unsigned	capture_flags;
} PlanReplayIdentityEntry;

typedef struct PlanReplayCostEntry
{
	PlanReplayPathId id;
	Cost		startup_cost;
	Cost		total_cost;
	bool		matched;
} PlanReplayCostEntry;

typedef struct PlanReplayCardinalityEntry
{
	PlanReplayCardinalityId id;
	Cardinality rows;
	bool		matched;
} PlanReplayCardinalityEntry;

typedef struct PlanReplayPruneEntry
{
	PlanReplayPathId id;
	bool		matched;
} PlanReplayPruneEntry;

static int plan_replay_mode = PLAN_REPLAY_MODE_OFF;
static int plan_replay_path_mode = PLAN_REPLAY_CHANNEL_INHERIT;
static int plan_replay_cardinality_mode = PLAN_REPLAY_CHANNEL_INHERIT;
static char *plan_replay_path_capture_file = NULL;
static char *plan_replay_path_replay_file = NULL;
static char *plan_replay_path_prune_file = NULL;
static char *plan_replay_cardinality_replay_file = NULL;

static planner_hook_type previous_planner_hook = NULL;
static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static cardinality_estimate_hook_type previous_cardinality_estimate_hook = NULL;

static FILE *plan_replay_path_capture_stream = NULL;
static FILE *plan_replay_path_replay_stream = NULL;
static FILE *plan_replay_path_prune_stream = NULL;
static FILE *plan_replay_cardinality_replay_stream = NULL;
static PlannerInfo *plan_replay_planner_root = NULL;
static uint64 plan_replay_path_sequence = 0;
static uint64 plan_replay_path_capture_records = 0;
static uint64 plan_replay_batch_count = 0;
static uint64 plan_replay_last_path_count = 0;
static uint64 plan_replay_last_batch_count = 0;
/* 仅作为本轮 EXPLAIN 的诊断定位号；稀疏回放绝不按它匹配。 */
static uint64 plan_replay_cardinality_sequence = 0;
static uint64 plan_replay_cost_replay_entry_count = 0;
static uint64 plan_replay_cost_replay_applied_count = 0;
static uint64 plan_replay_cost_replay_matched_entry_count = 0;
static uint64 plan_replay_last_cost_replay_entry_count = 0;
static uint64 plan_replay_last_cost_replay_applied_count = 0;
static uint64 plan_replay_last_cost_replay_unmatched_count = 0;
static uint64 plan_replay_path_prune_entry_count = 0;
static uint64 plan_replay_path_prune_applied_count = 0;
static uint64 plan_replay_path_prune_matched_entry_count = 0;
static uint64 plan_replay_last_path_prune_entry_count = 0;
static uint64 plan_replay_last_path_prune_applied_count = 0;
static uint64 plan_replay_last_path_prune_unmatched_count = 0;
static uint64 plan_replay_cardinality_replay_entry_count = 0;
static uint64 plan_replay_cardinality_replay_applied_count = 0;
static uint64 plan_replay_cardinality_replay_matched_entry_count = 0;
static uint64 plan_replay_last_cardinality_replay_entry_count = 0;
static uint64 plan_replay_last_cardinality_replay_applied_count = 0;
static uint64 plan_replay_last_cardinality_replay_unmatched_count = 0;
static double plan_replay_last_planning_ms = 0;
static PlanReplayOnlineStats plan_replay_current_online_stats;
static PlanReplayOnlineStats plan_replay_last_online_stats;
static int plan_replay_planner_depth = 0;
static bool plan_replay_statement_active = false;
static HTAB *plan_replay_identity_table = NULL;
static MemoryContext plan_replay_identity_context = NULL;
static HTAB *plan_replay_cost_table = NULL;
static HTAB *plan_replay_prune_table = NULL;
static HTAB *plan_replay_cardinality_table = NULL;


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
	{"prune", PLAN_REPLAY_CHANNEL_PRUNE, false},
	{"capture_prune", PLAN_REPLAY_CHANNEL_CAPTURE_PRUNE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry plan_replay_cardinality_mode_options[] = {
	{"inherit", PLAN_REPLAY_CHANNEL_INHERIT, false},
	{"off", PLAN_REPLAY_CHANNEL_OFF, false},
	{"replay", PLAN_REPLAY_CHANNEL_REPLAY, false},
	{NULL, 0, false}
};

static const char *plan_replay_path_node_name(Path *path);
static PlanReplayPathId plan_replay_compute_path_id(Path *path);
static void plan_replay_format_path_id(PlanReplayPathId id, char *output);
static void plan_replay_format_stats(char *json, size_t size);
static void plan_replay_load_costs(void);
static void plan_replay_load_prunes(void);
static void plan_replay_load_cardinalities(void);
static int plan_replay_effective_channel_mode(int channel_mode);
static bool plan_replay_mode_captures(int mode);
static bool plan_replay_mode_replays(int mode);
static bool plan_replay_mode_predicts(int mode);
static bool plan_replay_mode_prunes(int mode);
static bool plan_replay_is_active(void);
static List *plan_replay_process_paths(RelOptInfo *rel, List *paths, bool partial);
static Cardinality plan_replay_cardinality_estimate(PlannerInfo *root,
													CardinalityEstimateContext *context);

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
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_PREDICT ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_PRUNE;
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

static bool
plan_replay_mode_prunes(int mode)
{
	return mode == PLAN_REPLAY_CHANNEL_PRUNE ||
		mode == PLAN_REPLAY_CHANNEL_CAPTURE_PRUNE;
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
plan_replay_append_descriptor_pathkeys(StringInfo output, const char *name, List *keys)
{
	ListCell *cell;

	appendStringInfo(output, "|%s=[", name);
	foreach(cell, keys)
	{
		PathKey *key = lfirst_node(PathKey, cell);
		EquivalenceClass *ec = key->pk_eclass;
		ListCell *member_cell;
		char **members;
		int count = 0;
		int i;

		while (ec->ec_merged)
			ec = ec->ec_merged;
		members = palloc(sizeof(char *) * Max(1, list_length(ec->ec_members)));
		foreach(member_cell, ec->ec_members)
		{
			EquivalenceMember *member = lfirst_node(EquivalenceMember, member_cell);
			StringInfoData value;

			/* Child translations are planner bookkeeping, not new equivalences. */
			if (member->em_is_child)
				continue;
			initStringInfo(&value);
			appendStringInfo(&value, "type=%u|relids=", member->em_datatype);
			plan_replay_append_relids(&value, member->em_relids);
			plan_replay_append_descriptor_node(&value, "expr", member->em_expr);
			members[count++] = value.data;
		}
		qsort(members, count, sizeof(char *), plan_replay_string_compare);
		appendStringInfo(output, "{family=%u|collation=%u|direction=%d|nulls_first=%d|members=[",
						 key->pk_opfamily, ec->ec_collation, (int) key->pk_cmptype,
						 key->pk_nulls_first ? 1 : 0);
		for (i = 0; i < count; i++)
		{
			appendStringInfo(output, "%zu:%s;", strlen(members[i]), members[i]);
			pfree(members[i]);
		}
		pfree(members);
		appendStringInfoString(output, "]};");
	}
	appendStringInfoChar(output, ']');
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
		entry->capture_written = false;
		entry->capture_flags = 0;
		/* GEQO deletes its per-tour context; identities live for the statement. */
		entry->descriptor = MemoryContextStrdup(plan_replay_identity_context,
											  descriptor);
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
				 errmsg("Path type %s is not supported by stable Path ID format 2",
						plan_replay_path_node_name(path)),
				 errdetail("Cost capture and replay fail closed instead of falling back to sequence matching.")));

	appendStringInfo(output,
				 "pg_plan_replay_path_id_v2|node_tag=%d|pathtype=%d"
				 "|parallel_aware=%d|parallel_safe=%d|parallel_workers=%d",
				 (int) nodeTag(path), (int) path->pathtype,
				 path->parallel_aware ? 1 : 0, path->parallel_safe ? 1 : 0,
				 path->parallel_workers);
	appendStringInfoString(output, "|parent_relids=");
	plan_replay_append_relids(output, parent_relids);
	plan_replay_append_descriptor_relations(output, parent_relids);
	appendStringInfoString(output, "|required_outer=");
	plan_replay_append_relids(output, required_outer);
	plan_replay_append_descriptor_pathkeys(output, "pathkeys", path->pathkeys);
	if (path->param_info != NULL)
		plan_replay_append_descriptor_clause_set(output, "parameter_clauses",
											 path->param_info->ppi_clauses, true);
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
				plan_replay_append_descriptor_pathkeys(output, "outer_sortkeys",
											  merge->outersortkeys);
				plan_replay_append_descriptor_pathkeys(output, "inner_sortkeys",
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
					 errmsg("Path type %s is not supported by stable Path ID format 2",
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

/* The capture contains only candidate identity, row scope and intervention hits.
 * Reuse the identity table to suppress duplicates; never retain Path pointers. */
static void
plan_replay_capture_path(Path *path, bool partial,
                         bool replay_applied, bool prune_applied)
{
	PlanReplayPathId id = {path->plan_replay_path_id_hi,
		path->plan_replay_path_id_lo};
	PlanReplayIdentityEntry *entry;
	unsigned flags = (partial ? 1 : 0) | (replay_applied ? 2 : 0) |
		(prune_applied ? 4 : 0);
	StringInfoData line;
	char text[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

	entry = hash_search(plan_replay_identity_table, &id, HASH_FIND, NULL);
	Assert(entry != NULL);
	flags |= entry->capture_flags;
	if (entry->capture_written && flags == entry->capture_flags)
		return;
	plan_replay_format_path_id(id, text);
	initStringInfo(&line);
	appendStringInfo(&line, "{\"path_id\":\"%s\",\"parallel_workers\":%d",
					 text, path->parallel_workers);
	if (flags & 1)
		appendStringInfoString(&line, ",\"partial\":true");
	if (flags & 2)
		appendStringInfoString(&line, ",\"cost_replay_applied\":true");
	if (flags & 4)
		appendStringInfoString(&line, ",\"path_prune_applied\":true");
	appendStringInfoString(&line, "}\n");
	if (fwrite(line.data, 1, line.len, plan_replay_path_capture_stream) != (size_t) line.len)
		ereport(ERROR, (errcode_for_file_access(),
						 errmsg("could not write pg_plan_replay Path capture: %m")));
	pfree(line.data);
	entry->capture_written = true;
	entry->capture_flags = flags;
	plan_replay_path_capture_records++;
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
			if (strcmp(cursor, "# pg_plan_replay_costs format=3") != 0 ||
				header_seen || line_number != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid pg_plan_replay cost replay header at line "
								UINT64_FORMAT, line_number),
						 errdetail("Expected exactly: # pg_plan_replay_costs format=3"),
						 errhint("Path ID v2 requires new captures; recapture legacy Path replay files.")));
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
					 errmsg("pg_plan_replay cost replay file has no format 3 header"),
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
				 errdetail("Expected header: # pg_plan_replay_costs format=3")));
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

/*
 * 读取只包含稳定物理 Path ID 的稀疏剪枝文件。剪枝文件不携带 cost，命中时
 * 直接阻止该候选进入 PostgreSQL 原生 add_path() 竞赛。
 */
static void
plan_replay_load_prunes(void)
{
	char line[PLAN_REPLAY_PRUNE_LINE_MAX];
	uint64 line_number = 0;
	bool header_seen = false;

	Assert(plan_replay_path_prune_stream != NULL);
	Assert(plan_replay_prune_table != NULL);
	while (fgets(line, sizeof(line), plan_replay_path_prune_stream) != NULL)
	{
		char *cursor = line;
		PlanReplayPathId id;
		PlanReplayPruneEntry *entry;
		bool found;

		line_number++;
		if (strchr(line, '\n') == NULL && !feof(plan_replay_path_prune_stream))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay Path prune line " UINT64_FORMAT " is too long",
							line_number)));
		while (*cursor != '\0' && isspace((unsigned char) *cursor))
			cursor++;
		if (*cursor == '#')
		{
			char *newline = strchr(cursor, '\n');

			if (newline != NULL)
				*newline = '\0';
			if (strcmp(cursor, "# pg_plan_replay_prune format=2") != 0 ||
				header_seen || line_number != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid pg_plan_replay Path prune header at line "
								UINT64_FORMAT, line_number),
						 errdetail("Expected exactly: # pg_plan_replay_prune format=2"),
						 errhint("Path ID v2 requires new captures; recapture legacy Path replay files.")));
			header_seen = true;
			continue;
		}
		if (*cursor == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("empty pg_plan_replay Path prune line " UINT64_FORMAT,
							line_number)));
		if (!header_seen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay Path prune file has no format 2 header")));

		if (strlen(cursor) < PLAN_REPLAY_PATH_ID_HEX_LENGTH ||
			!plan_replay_parse_path_id(cursor, &id))
			goto invalid_line;
		cursor += PLAN_REPLAY_PATH_ID_HEX_LENGTH;
		while (*cursor != '\0' && isspace((unsigned char) *cursor))
			cursor++;
		if (*cursor != '\0')
			goto invalid_line;

		entry = hash_search(plan_replay_prune_table, &id, HASH_ENTER, &found);
		if (found)
		{
			char path_id[PLAN_REPLAY_PATH_ID_HEX_LENGTH + 1];

			plan_replay_format_path_id(id, path_id);
			ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("duplicate stable Path ID %s in Path prune file",
								path_id)));
		}
		entry->matched = false;
		plan_replay_path_prune_entry_count++;

		continue;

invalid_line:
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("malformed pg_plan_replay Path prune line " UINT64_FORMAT,
						line_number),
				 errdetail("Expected exactly: 32_hex_path_id")));
	}
	if (ferror(plan_replay_path_prune_stream))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read pg_plan_replay Path prune file: %m")));
	if (!header_seen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay Path prune file is empty"),
				 errdetail("Expected header: # pg_plan_replay_prune format=2")));
}

static bool
plan_replay_prune_path(Path *path)
{
	PlanReplayPathId id;
	PlanReplayPruneEntry *entry;

	Assert(path->plan_replay_path_id_valid);
	id.hi = path->plan_replay_path_id_hi;
	id.lo = path->plan_replay_path_id_lo;
	entry = hash_search(plan_replay_prune_table, &id, HASH_FIND, NULL);
	if (entry == NULL)
		return false;
	if (!entry->matched)
	{
		entry->matched = true;
		plan_replay_path_prune_matched_entry_count++;
	}
	plan_replay_path_prune_applied_count++;
	return true;
}

static bool
plan_replay_parse_cardinality_id(const char *text, PlanReplayCardinalityId *id)
{
	int index;

	id->hi = 0;
	id->lo = 0;
	for (index = 0; index < 32; index++)
	{
		int value;

		if (text[index] >= '0' && text[index] <= '9')
			value = text[index] - '0';
		else if (text[index] >= 'a' && text[index] <= 'f')
			value = text[index] - 'a' + 10;
		else
			return false;
		if (index < 16)
			id->hi = (id->hi << 4) | value;
		else
			id->lo = (id->lo << 4) | value;
	}
	return text[32] == '\0' || isspace((unsigned char) text[32]);
}

static void
plan_replay_load_cardinalities(void)
{
	char line[PLAN_REPLAY_CARDINALITY_LINE_MAX];
	uint64 line_number = 0;
	bool header_seen = false;

	/*
	 * 在语句规划开始时一次性读入 ID -> rows 哈希表。缺失 ID 保留原生估算；
	 * 重复、旧顺序格式或非法数值立即报错，避免带病回放。
	 */
	Assert(plan_replay_cardinality_replay_stream != NULL);
	Assert(plan_replay_cardinality_table != NULL);
	while (fgets(line, sizeof(line), plan_replay_cardinality_replay_stream) != NULL)
	{
		char *cursor = line;
		char *end;
		PlanReplayCardinalityId id;
		PlanReplayCardinalityEntry *entry;
		double rows;
		bool found;

		line_number++;
		if (strchr(line, '\n') == NULL &&
			!feof(plan_replay_cardinality_replay_stream))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay cardinality replay line " UINT64_FORMAT
							" is too long", line_number)));
		while (*cursor != '\0' && isspace((unsigned char) *cursor))
			cursor++;
		if (*cursor == '#')
		{
			char *newline = strchr(cursor, '\n');

			if (newline != NULL)
				*newline = '\0';
			if (strcmp(cursor, "# pg_plan_replay_cardinalities format=2") != 0 ||
				header_seen || line_number != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid pg_plan_replay cardinality replay header at line "
								UINT64_FORMAT, line_number),
						 errdetail("Expected exactly: # pg_plan_replay_cardinalities format=2")));
			header_seen = true;
			continue;
		}
		if (*cursor == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("empty pg_plan_replay cardinality replay line " UINT64_FORMAT,
							line_number)));
		if (!header_seen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("pg_plan_replay cardinality replay file has no format 2 header"),
					 errhint("Regenerate the sparse .rows file; sequence-based files are not supported.")));

		if (strlen(cursor) < 32 ||
			!plan_replay_parse_cardinality_id(cursor, &id))
			goto invalid_line;
		cursor += 32;
		if (!isspace((unsigned char) *cursor))
			goto invalid_line;
		while (isspace((unsigned char) *cursor))
			cursor++;
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
					 errmsg("invalid pg_plan_replay cardinality at line " UINT64_FORMAT,
							line_number),
					 errdetail("Rows must be finite and nonnegative.")));

		entry = hash_search(plan_replay_cardinality_table, &id, HASH_ENTER, &found);
		if (found)
		{
			char text[33];

			plan_replay_format_cardinality_id(id, text);
			ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("duplicate cardinality ID %s in replay file", text)));
		}
		entry->rows = rows;
		entry->matched = false;
		plan_replay_cardinality_replay_entry_count++;

		continue;

	invalid_line:
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("malformed pg_plan_replay cardinality replay line "
						UINT64_FORMAT, line_number),
				 errdetail("Expected exactly: 32_lowercase_hex_cardinality_id rows")));
	}
	if (ferror(plan_replay_cardinality_replay_stream))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read pg_plan_replay cardinality replay file: %m")));
	if (!header_seen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pg_plan_replay cardinality replay file is empty"),
				 errdetail("Expected header: # pg_plan_replay_cardinalities format=2")));
}

static Cardinality
plan_replay_replay_cardinality(PlanReplayCardinalityId id,
							   Cardinality native_rows)
{
	PlanReplayCardinalityEntry *entry;

	/* 一个语义 ID 可对应多次估算调用；每次命中都注入同一个 rows。 */
	entry = hash_search(plan_replay_cardinality_table, &id, HASH_FIND, NULL);
	if (entry == NULL)
		return native_rows;
	if (!entry->matched)
	{
		entry->matched = true;
		plan_replay_cardinality_replay_matched_entry_count++;
	}
	plan_replay_cardinality_replay_applied_count++;
	return entry->rows;
}

static Cardinality
plan_replay_cardinality_estimate(PlannerInfo *root,
							 CardinalityEstimateContext *context)
{
	Cardinality rows = context->native_rows;
	PlanReplayCardinalityId id;

	/* 先尊重 hook 链前面的估算结果，再把它视作本扩展看到的 native_rows。 */
	if (previous_cardinality_estimate_hook != NULL)
		rows = previous_cardinality_estimate_hook(root, context);
	if (!plan_replay_statement_active)
		return rows;

	plan_replay_planner_root = root;
	/* ID 在注入 rows 之前计算，且其描述符不包含任何行数。 */
	id = plan_replay_compute_cardinality_id(root, context);
	context->cardinality_id_valid = true;
	context->cardinality_id_hi = id.hi;
	context->cardinality_id_lo = id.lo;
	if (plan_replay_cardinality_table != NULL)
		rows = plan_replay_replay_cardinality(id, rows);

	/* sequence 仅在 ID 匹配完成后写回，确保它不可能影响身份。 */
	context->sequence_valid = true;
	context->sequence = plan_replay_cardinality_sequence;
	plan_replay_cardinality_sequence++;
	return rows;
}

static bool
plan_replay_is_active(void)
{
	int path_mode = plan_replay_effective_channel_mode(plan_replay_path_mode);

	return plan_replay_path_capture_stream != NULL ||
		plan_replay_path_replay_stream != NULL ||
		plan_replay_path_prune_stream != NULL ||
		plan_replay_mode_predicts(path_mode) ||
		plan_replay_mode_prunes(path_mode);
}

static void
plan_replay_propagate_path_cardinality(Path *path)
{
	/*
	 * Path 通道会在 PostgreSQL 原生 add_path() 前暂存候选 Path，所以这里也要
	 * 传播一次基数身份；否则 capture 的候选 Path JSON 会暂时看不到 ID。
	 * 参数化 Path 优先取 ParamPathInfo，普通 Path 取父 RelOptInfo。
	 */
	path->plan_replay_cardinality_sequence_valid = false;
	path->plan_replay_cardinality_sequence = 0;
	path->plan_replay_cardinality_id_valid = false;
	path->plan_replay_cardinality_id_hi = 0;
	path->plan_replay_cardinality_id_lo = 0;
	if (path->param_info != NULL)
	{
		path->plan_replay_cardinality_sequence_valid =
			path->param_info->plan_replay_cardinality_sequence_valid;
		path->plan_replay_cardinality_sequence =
			path->param_info->plan_replay_cardinality_sequence;
		path->plan_replay_cardinality_id_valid =
			path->param_info->plan_replay_cardinality_id_valid;
		path->plan_replay_cardinality_id_hi =
			path->param_info->plan_replay_cardinality_id_hi;
		path->plan_replay_cardinality_id_lo =
			path->param_info->plan_replay_cardinality_id_lo;
	}
	else if (path->parent != NULL)
	{
		path->plan_replay_cardinality_sequence_valid =
			path->parent->plan_replay_cardinality_sequence_valid;
		path->plan_replay_cardinality_sequence =
			path->parent->plan_replay_cardinality_sequence;
		path->plan_replay_cardinality_id_valid =
			path->parent->plan_replay_cardinality_id_valid;
		path->plan_replay_cardinality_id_hi =
			path->parent->plan_replay_cardinality_id_hi;
		path->plan_replay_cardinality_id_lo =
			path->parent->plan_replay_cardinality_id_lo;
	}
}

static List *
plan_replay_process_paths(RelOptInfo *rel, List *paths, bool partial)
{
	ListCell *cell;
	bool *replay_applied;
	bool *prune_applied;
	int path_count = list_length(paths);
	int path_mode = plan_replay_effective_channel_mode(plan_replay_path_mode);
	bool path_channel_active = plan_replay_path_capture_stream != NULL ||
		plan_replay_path_replay_stream != NULL ||
		plan_replay_prune_table != NULL ||
		plan_replay_mode_predicts(path_mode);
	int index = 0;

	if (path_count == 0)
		return paths;
	if (!path_channel_active)
		return paths;
	replay_applied = palloc0(sizeof(bool) * path_count);
	prune_applied = palloc0(sizeof(bool) * path_count);

	plan_replay_batch_count++;
	foreach(cell, paths)
	{
		Path *path = (Path *) lfirst(cell);


		path->plan_replay_path_sequence_valid = true;
		path->plan_replay_path_sequence = plan_replay_path_sequence;
		{
			PlanReplayPathId id = plan_replay_compute_path_id(path);

			path->plan_replay_path_id_valid = true;
			path->plan_replay_path_id_hi = id.hi;
			path->plan_replay_path_id_lo = id.lo;
		}
		plan_replay_propagate_path_cardinality(path);
		if (plan_replay_cost_table != NULL)
			replay_applied[index] = plan_replay_replay_path(path);
		if (plan_replay_prune_table != NULL)
			prune_applied[index] = plan_replay_prune_path(path);
		index++;
	}

	if (plan_replay_mode_predicts(path_mode))
		(void) plan_replay_online_predict_paths(plan_replay_planner_root,
												  rel, paths, partial,
												  &plan_replay_current_online_stats);

	index = 0;
	foreach(cell, paths)
	{
		Path *path = (Path *) lfirst(cell);

		if (plan_replay_path_capture_stream != NULL)
			plan_replay_capture_path(path, partial, replay_applied[index],
								 prune_applied[index]);
		plan_replay_path_sequence++;
		index++;
	}

	/*
	 * capture 完整记录命中候选后，再从待进入原生 add_path() 的列表中删除。
	 * Path 本体留在 planner memory context，由语句结束统一释放，避免误释放被
	 * BitmapHeapPath 等对象引用的 IndexPath。
	 */
	index = 0;
	foreach(cell, paths)
	{
		if (prune_applied[index])
			paths = foreach_delete_current(paths, cell);
		index++;
	}
	pfree(prune_applied);
	pfree(replay_applied);
	return paths;
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
plan_replay_create_path_tables(bool replay_paths, bool prune_paths)
{
	HASHCTL control;

	plan_replay_identity_context = CurrentMemoryContext;
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
	if (prune_paths)
	{
		MemSet(&control, 0, sizeof(control));
		control.keysize = sizeof(PlanReplayPathId);
		control.entrysize = sizeof(PlanReplayPruneEntry);
		plan_replay_prune_table =
			hash_create("pg_plan_replay Path prune IDs", 128,
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
		plan_replay_identity_context = NULL;
	}
	if (plan_replay_cost_table != NULL)
	{
		hash_destroy(plan_replay_cost_table);
		plan_replay_cost_table = NULL;
	}
	if (plan_replay_prune_table != NULL)
	{
		hash_destroy(plan_replay_prune_table);
		plan_replay_prune_table = NULL;
	}
	if (plan_replay_cardinality_table != NULL)
	{
		hash_destroy(plan_replay_cardinality_table);
		plan_replay_cardinality_table = NULL;
	}
	plan_replay_cardinality_identity_end();
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
	bool prune_paths = plan_replay_mode_prunes(path_mode);
	bool replay_cardinality = plan_replay_mode_replays(cardinality_mode);

	plan_replay_statement_active = false;
	if (!capture_paths && !replay_paths && !predict_paths && !prune_paths &&
		!replay_cardinality)
		return;
	plan_replay_statement_active = true;


	if (capture_paths)
		plan_replay_require_absolute_file(plan_replay_path_capture_file,
									  "Path capture");
	if (replay_paths)
		plan_replay_require_absolute_file(plan_replay_path_replay_file,
									  "Path replay");
	if (prune_paths)
		plan_replay_require_absolute_file(plan_replay_path_prune_file,
									  "Path prune");
	if (replay_cardinality)
		plan_replay_require_absolute_file(plan_replay_cardinality_replay_file,
									  "cardinality replay");

	if (capture_paths && replay_paths)
		plan_replay_require_distinct_files(plan_replay_path_capture_file,
									   "Path capture",
									   plan_replay_path_replay_file,
									   "Path replay");
	if (capture_paths && prune_paths)
		plan_replay_require_distinct_files(plan_replay_path_capture_file,
									   "Path capture",
									   plan_replay_path_prune_file,
									   "Path prune");
	if (replay_paths && prune_paths)
		plan_replay_require_distinct_files(plan_replay_path_replay_file,
									   "Path replay",
									   plan_replay_path_prune_file,
									   "Path prune");
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
	if (prune_paths && replay_cardinality)
		plan_replay_require_distinct_files(plan_replay_path_prune_file,
									   "Path prune",
									   plan_replay_cardinality_replay_file,
									   "cardinality replay");

	plan_replay_path_sequence = 0;
	plan_replay_path_capture_records = 0;
	plan_replay_batch_count = 0;
	plan_replay_cardinality_sequence = 0;
	plan_replay_cost_replay_entry_count = 0;
	plan_replay_cost_replay_applied_count = 0;
	plan_replay_cost_replay_matched_entry_count = 0;
	plan_replay_path_prune_entry_count = 0;
	plan_replay_path_prune_applied_count = 0;
	plan_replay_path_prune_matched_entry_count = 0;
	plan_replay_cardinality_replay_entry_count = 0;
	plan_replay_cardinality_replay_applied_count = 0;
	plan_replay_cardinality_replay_matched_entry_count = 0;
	plan_replay_online_reset_stats(&plan_replay_current_online_stats);
	plan_replay_planner_root = NULL;
	if (capture_paths || replay_paths || predict_paths || prune_paths)
		plan_replay_create_path_tables(replay_paths, prune_paths);
	/* Compute logical IDs for EXPLAIN whenever either channel is active. */
	plan_replay_cardinality_identity_begin(replay_cardinality);
	if (replay_cardinality)
	{
		HASHCTL control;

		MemSet(&control, 0, sizeof(control));
		control.keysize = sizeof(PlanReplayCardinalityId);
		control.entrysize = sizeof(PlanReplayCardinalityEntry);
		plan_replay_cardinality_table =
			hash_create("pg_plan_replay sparse cardinalities", 128,
						&control, HASH_ELEM | HASH_BLOBS);
	}

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
	if (prune_paths)
	{
		plan_replay_path_prune_stream =
			AllocateFile(plan_replay_path_prune_file, "r");
		if (plan_replay_path_prune_stream == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay Path prune file \"%s\": %m",
							plan_replay_path_prune_file)));
		plan_replay_load_prunes();
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
		plan_replay_load_cardinalities();
	}
	if (capture_paths)
	{
		plan_replay_path_capture_stream =
			AllocateFile(plan_replay_path_capture_file, "a");
		if (plan_replay_path_capture_stream == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open pg_plan_replay Path capture file \"%s\": %m",
							plan_replay_path_capture_file)));
		fprintf(plan_replay_path_capture_stream,
				"{\"record\":\"header\",\"format_version\":9,\"path_id_version\":2,"
				"\"parallel_leader_participation\":%s}\n",
				parallel_leader_participation ? "true" : "false");
	}
}

static void
plan_replay_abort_statement(void)
{
	FILE *path_capture = plan_replay_path_capture_stream;
	FILE *path_replay = plan_replay_path_replay_stream;
	FILE *path_prune = plan_replay_path_prune_stream;
	FILE *cardinality_replay = plan_replay_cardinality_replay_stream;

	plan_replay_path_capture_stream = NULL;
	plan_replay_path_replay_stream = NULL;
	plan_replay_path_prune_stream = NULL;
	plan_replay_cardinality_replay_stream = NULL;

	plan_replay_destroy_path_tables();
	plan_replay_planner_root = NULL;
	plan_replay_statement_active = false;
	if (path_capture != NULL)
		FreeFile(path_capture);
	if (path_replay != NULL)
		FreeFile(path_replay);
	if (path_prune != NULL)
		FreeFile(path_prune);
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
	uint64 unmatched_prunes = plan_replay_path_prune_entry_count -
		plan_replay_path_prune_matched_entry_count;
	uint64 unmatched_cardinalities = plan_replay_cardinality_replay_entry_count -
		plan_replay_cardinality_replay_matched_entry_count;

	if (!plan_replay_statement_active)
		return;
	plan_replay_last_path_count = plan_replay_path_sequence;
	plan_replay_last_batch_count = plan_replay_batch_count;
	plan_replay_last_cost_replay_entry_count =
		plan_replay_cost_replay_entry_count;
	plan_replay_last_cost_replay_applied_count =
		plan_replay_cost_replay_applied_count;
	plan_replay_last_cost_replay_unmatched_count = unmatched_costs;
	plan_replay_last_path_prune_entry_count =
		plan_replay_path_prune_entry_count;
	plan_replay_last_path_prune_applied_count =
		plan_replay_path_prune_applied_count;
	plan_replay_last_path_prune_unmatched_count = unmatched_prunes;
	plan_replay_last_cardinality_replay_entry_count =
		plan_replay_cardinality_replay_entry_count;
	plan_replay_last_cardinality_replay_applied_count =
		plan_replay_cardinality_replay_applied_count;
	plan_replay_last_cardinality_replay_unmatched_count =
		unmatched_cardinalities;
	plan_replay_last_online_stats = plan_replay_current_online_stats;
	if (plan_replay_path_capture_stream != NULL)
	{
		char stats[1024];

		plan_replay_format_stats(stats, sizeof(stats));
		fprintf(plan_replay_path_capture_stream,
				"{\"record\":\"end\",\"records\":" UINT64_FORMAT
				",\"stats\":%s}\n", plan_replay_path_capture_records, stats);
	}
	plan_replay_finish_file(&plan_replay_path_replay_stream, true, "Path cost",
					plan_replay_path_sequence, "Path");
	plan_replay_finish_file(&plan_replay_path_capture_stream, false, "Path cost",
					plan_replay_path_sequence, "Path");
	plan_replay_finish_file(&plan_replay_path_prune_stream, true, "Path prune",
					plan_replay_path_prune_entry_count, "entry");
	plan_replay_finish_file(&plan_replay_cardinality_replay_stream, true,
						"cardinality", plan_replay_cardinality_sequence, "estimate");
	plan_replay_planner_root = NULL;
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

			if (statement_was_active)
			{
				INSTR_TIME_SET_CURRENT(elapsed);
				INSTR_TIME_SUBTRACT(elapsed, started);
				plan_replay_last_planning_ms = INSTR_TIME_GET_MILLISEC(elapsed);
			}
			plan_replay_finish_statement();
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

static void
plan_replay_format_stats(char *json, size_t size)
{
	snprintf(json, size,
			 "{\"path_id_version\":2,\"path_capture_version\":9,\"rows_format\":2,\"cost_format\":3,\"prune_format\":2,\"planning_ms\":%.6f,\"execution_ms\":0,\"inference_ms\":%.6f,"
			 "\"batches\":" UINT64_FORMAT ",\"paths\":" UINT64_FORMAT
			 ",\"model_batches\":" UINT64_FORMAT
			 ",\"model_paths\":" UINT64_FORMAT
			 ",\"model_applied\":" UINT64_FORMAT
			 ",\"cost_replay_entries\":" UINT64_FORMAT
			 ",\"cost_replay_applied\":" UINT64_FORMAT
			 ",\"cost_replay_unmatched\":" UINT64_FORMAT
			 ",\"path_prune_entries\":" UINT64_FORMAT
			 ",\"path_prune_applied\":" UINT64_FORMAT
			 ",\"path_prune_unmatched\":" UINT64_FORMAT
			 ",\"cardinality_replay_entries\":" UINT64_FORMAT
			 ",\"cardinality_replay_applied\":" UINT64_FORMAT
			 ",\"cardinality_replay_unmatched\":" UINT64_FORMAT
			 ",\"fallbacks\":0}",
			 plan_replay_last_planning_ms,
			 plan_replay_last_online_stats.inference_ms,
			 plan_replay_last_batch_count, plan_replay_last_path_count,
			 plan_replay_last_online_stats.batches,
			 plan_replay_last_online_stats.paths,
			 plan_replay_last_online_stats.applied,
			 plan_replay_last_cost_replay_entry_count,
			 plan_replay_last_cost_replay_applied_count,
			 plan_replay_last_cost_replay_unmatched_count,
			 plan_replay_last_path_prune_entry_count,
			 plan_replay_last_path_prune_applied_count,
			 plan_replay_last_path_prune_unmatched_count,
			 plan_replay_last_cardinality_replay_entry_count,
			 plan_replay_last_cardinality_replay_applied_count,
			 plan_replay_last_cardinality_replay_unmatched_count);
}

Datum
plan_replay_last_timing(PG_FUNCTION_ARGS)
{
	char json[1024];

	plan_replay_format_stats(json, sizeof(json));
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}

Datum
plan_replay_reset_stats(PG_FUNCTION_ARGS)
{
	plan_replay_path_sequence = 0;
	plan_replay_path_capture_records = 0;
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
	plan_replay_path_prune_entry_count = 0;
	plan_replay_path_prune_applied_count = 0;
	plan_replay_path_prune_matched_entry_count = 0;
	plan_replay_last_path_prune_entry_count = 0;
	plan_replay_last_path_prune_applied_count = 0;
	plan_replay_last_path_prune_unmatched_count = 0;
	plan_replay_cardinality_replay_entry_count = 0;
	plan_replay_cardinality_replay_applied_count = 0;
	plan_replay_cardinality_replay_matched_entry_count = 0;
	plan_replay_last_cardinality_replay_entry_count = 0;
	plan_replay_last_cardinality_replay_applied_count = 0;
	plan_replay_last_cardinality_replay_unmatched_count = 0;
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
	DefineCustomStringVariable("plan_replay.path_prune_file",
						   "Absolute sparse stable-ID Path prune input path for one statement.",
						   NULL,
						   &plan_replay_path_prune_file,
						   "",
						   PGC_SUSET,
						   0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("plan_replay.cardinality_replay_file",
						   "Absolute sparse stable-ID cardinality input path for one statement.",
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
