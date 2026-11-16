/* Concrete scan contexts and bitmap metadata belong to the replay extension. */
#include "postgres.h"

#include "catalog/pg_am_d.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/memutils.h"

#include "path_binding.h"

typedef struct BoundPPI
{
	RelOptInfo *rel;
	ParamPathInfo *ppi;
	uint64 driver_hi, driver_lo;
} BoundPPI;

static MemoryContext binding_context;
static List *bound_cache;
static parameterized_scan_bind_hook_type previous_bind_hook;
static index_cardinality_hook_type previous_index_hook;
static bitmap_index_plan_hook_type previous_bitmap_hook;
static explain_per_node_hook_type previous_explain_hook;

static Path *bind_scan_context(PlannerInfo *root, Path *inner_path,
							  Path *outer_path, bool tuple_candidate);

/* A binding clone must not inherit a descriptor cached for another driver. */
static void
set_path_physical_driver(Path *path, Path *outer_path)
{
	path->plan_replay_physical_driver = outer_path;
	path->plan_replay_path_id_valid = false;
	path->plan_replay_path_id_hi = 0;
	path->plan_replay_path_id_lo = 0;
	path->plan_replay_path_sequence_valid = false;
	path->plan_replay_path_sequence = 0;
}

static Path *
process_bound_scan_path(Path *path, Path *outer_path, bool tuple_candidate)
{
	set_path_physical_driver(path, outer_path);
	plan_replay_propagate_path_cardinality(path);
	if (tuple_candidate && path_cost_model_active_hook &&
		path_cost_model_active_hook() &&
		path_cost_model_batch_hook)
	{
		List *processed = path_cost_model_batch_hook(path->parent,
													 list_make1(path), false);

		return processed == NIL ? NULL : linitial(processed);
	}
	return path;
}

/* Auxiliary bitmap nodes carry identities, but are not standalone tuple
 * candidates. Bind them without sending them through cost replay or pruning. */
static Path *
bind_bitmap_child(PlannerInfo *root, Path *child, Path *outer_path,
				  bool *changed)
{
	Path *bound = bind_scan_context(root, child, outer_path, false);
	Cost old_cost, new_cost;
	Selectivity old_selectivity, new_selectivity;

	cost_bitmap_tree_node(child, &old_cost, &old_selectivity);
	cost_bitmap_tree_node(bound, &new_cost, &new_selectivity);
	if (old_cost != new_cost || old_selectivity != new_selectivity)
		*changed = true;
	return bound;
}

/* Bind a reusable parameterized base scan to its actual input population.
 * Never overwrite the template PPI: other join orders still use it. */
static Path *
plan_replay_bind_scan(PlannerInfo *root, Path *inner_path,
								Path *outer_path)
{
	if (previous_bind_hook)
		inner_path = previous_bind_hook(root, inner_path, outer_path);
	return bind_scan_context(root, inner_path, outer_path, true);
}

static Path *
bind_scan_context(PlannerInfo *root, Path *inner_path,
				  Path *outer_path, bool tuple_candidate)
{
	ParamPathInfo *template_ppi;
	ParamPathInfo *bound_ppi = NULL;
	CardinalityEstimateContext context;
	RelOptInfo *rel;
	Path *result;
	ListCell *lc;
	bool changed;

	if (inner_path == NULL || binding_context == NULL)
		return inner_path;
	template_ppi = inner_path->param_info;
	rel = inner_path->parent;
	if (cardinality_estimate_hook == NULL || template_ppi == NULL ||
		!IS_SIMPLE_REL(rel) ||
		!bms_is_subset(template_ppi->ppi_req_outer, outer_path->parent->relids))
		return inner_path;
	if (inner_path->plan_replay_physical_driver == outer_path)
		return inner_path;

	/* Wrappers must keep the bound child and its PPI together. */
	if (IsA(inner_path, MemoizePath))
	{
		MemoizePath *old = (MemoizePath *) inner_path;
		Path *sub = bind_scan_context(root, old->subpath, outer_path, tuple_candidate);

		if (sub == NULL)
			return NULL;
		result = (Path *) create_memoize_path(root, rel, sub, old->param_exprs,
			old->hash_operators, old->singlerow, old->binary_mode, old->calls);
		return process_bound_scan_path(result, outer_path, tuple_candidate);
	}
	if (IsA(inner_path, MaterialPath))
	{
		MaterialPath *old = (MaterialPath *) inner_path;
		Path *sub = bind_scan_context(root, old->subpath, outer_path, tuple_candidate);

		if (sub == NULL)
			return NULL;
		result = (Path *) create_material_path(rel, sub);
		return process_bound_scan_path(result, outer_path, tuple_candidate);
	}
	if (!IsA(inner_path, IndexPath) && !IsA(inner_path, BitmapHeapPath) &&
		!(!tuple_candidate &&
		  (IsA(inner_path, BitmapAndPath) || IsA(inner_path, BitmapOrPath))) &&
		!(IsA(inner_path, Path) && inner_path->pathtype == T_SeqScan))
		return inner_path;

	plan_replay_propagate_path_cardinality(outer_path);
	if (!outer_path->plan_replay_cardinality_id_valid)
	{
		/* Physical identity can still bind when no logical driver ID exists. */
		bound_ppi = template_ppi;
		goto clone_scan;
	}

	foreach(lc, bound_cache)
	{
		BoundPPI *entry = lfirst(lc);

		if (entry->rel == rel &&
			entry->driver_hi == outer_path->plan_replay_cardinality_id_hi &&
			entry->driver_lo == outer_path->plan_replay_cardinality_id_lo &&
			bms_equal(entry->ppi->ppi_req_outer, template_ppi->ppi_req_outer))
		{
			bound_ppi = entry->ppi;
			break;
		}
	}
	if (bound_ppi == NULL)
	{
		Cardinality rows;
		MemoryContext oldcontext;
		BoundPPI *entry;

		MemSet(&context, 0, sizeof(context));
		context.kind = CARDINALITY_ESTIMATE_PARAMETERIZED_BASE;
		context.rel = rel;
		context.restrictlist = list_concat_copy(template_ppi->ppi_clauses,
											 rel->baserestrictinfo);
		context.required_outer = template_ppi->ppi_req_outer;
		context.native_rows = template_ppi->ppi_rows;
		context.driving_path = outer_path;
		rows = cardinality_estimate_hook(root, &context);
		rows = clamp_replayed_row_est(rows, context.preserve_fractional_rows);
		rows = Min(rows, rel->rows);
		oldcontext = MemoryContextSwitchTo(binding_context);
		bound_ppi = makeNode(ParamPathInfo);
		memcpy(bound_ppi, template_ppi, sizeof(ParamPathInfo));
		bound_ppi->ppi_rows = rows;
		bound_ppi->ppi_req_outer = bms_copy(template_ppi->ppi_req_outer);
		bound_ppi->plan_replay_preserve_fractional_rows = context.preserve_fractional_rows;
		bound_ppi->plan_replay_cardinality_sequence_valid = context.sequence_valid;
		bound_ppi->plan_replay_cardinality_sequence = context.sequence;
		bound_ppi->plan_replay_cardinality_id_valid = context.cardinality_id_valid;
		bound_ppi->plan_replay_cardinality_id_hi = context.cardinality_id_hi;
		bound_ppi->plan_replay_cardinality_id_lo = context.cardinality_id_lo;
		entry = palloc(sizeof(BoundPPI));
		entry->rel = rel;
		entry->ppi = bound_ppi;
		entry->driver_hi = outer_path->plan_replay_cardinality_id_hi;
		entry->driver_lo = outer_path->plan_replay_cardinality_id_lo;
		bound_cache = lappend(bound_cache, entry);
		MemoryContextSwitchTo(oldcontext);
		list_free(context.restrictlist);
	}

clone_scan:
	changed = bound_ppi->ppi_rows != template_ppi->ppi_rows ||
		inner_path->plan_replay_physical_driver != NULL;
	if (IsA(inner_path, IndexPath))
	{
		IndexPath *copy = makeNode(IndexPath);
		IndexPath *old = (IndexPath *) inner_path;

		memcpy(copy, old, sizeof(IndexPath));
		copy->path.param_info = bound_ppi;
		set_path_physical_driver(&copy->path, outer_path);
		cost_index(copy, root, Max(outer_path->rows, 1.0), false);
		/* Binding metadata alone must not recost an uninjected native path. */
		if (!changed && copy->indexselectivity == old->indexselectivity)
		{
			copy->path.startup_cost = old->path.startup_cost;
			copy->path.total_cost = old->path.total_cost;
			copy->indextotalcost = old->indextotalcost;
		}
		else
			changed = true;
		result = &copy->path;
	}
	else if (IsA(inner_path, BitmapHeapPath))
	{
		BitmapHeapPath *old = (BitmapHeapPath *) inner_path;
		BitmapHeapPath *copy = makeNode(BitmapHeapPath);

		memcpy(copy, old, sizeof(BitmapHeapPath));
		copy->path.param_info = bound_ppi;
		copy->path.rows = bound_ppi->ppi_rows;
		copy->bitmapqual = bind_bitmap_child(root, old->bitmapqual, outer_path, &changed);
		if (changed)
			cost_bitmap_heap_scan(&copy->path, root, rel, bound_ppi,
				copy->bitmapqual, Max(outer_path->rows, 1.0));
		result = &copy->path;
	}
	else if (IsA(inner_path, BitmapAndPath))
	{
		BitmapAndPath *old = (BitmapAndPath *) inner_path;
		BitmapAndPath *copy = makeNode(BitmapAndPath);

		memcpy(copy, old, sizeof(BitmapAndPath));
		copy->path.param_info = bound_ppi;
		copy->bitmapquals = NIL;
		foreach(lc, old->bitmapquals)
			copy->bitmapquals = lappend(copy->bitmapquals,
				bind_bitmap_child(root, lfirst(lc), outer_path, &changed));
		if (changed)
			cost_bitmap_and_node(copy, root);
		result = &copy->path;
	}
	else if (IsA(inner_path, BitmapOrPath))
	{
		BitmapOrPath *old = (BitmapOrPath *) inner_path;
		BitmapOrPath *copy = makeNode(BitmapOrPath);

		memcpy(copy, old, sizeof(BitmapOrPath));
		copy->path.param_info = bound_ppi;
		copy->bitmapquals = NIL;
		foreach(lc, old->bitmapquals)
			copy->bitmapquals = lappend(copy->bitmapquals,
				bind_bitmap_child(root, lfirst(lc), outer_path, &changed));
		if (changed)
			cost_bitmap_or_node(copy, root);
		result = &copy->path;
	}
	else
	{
		result = makeNode(Path);
		memcpy(result, inner_path, sizeof(Path));
		result->param_info = bound_ppi;
		result->rows = bound_ppi->ppi_rows;
		if (changed)
			cost_seqscan(result, root, rel, bound_ppi);
	}
	/* Physical binding is significant even if the logical rows are unchanged. */
	return process_bound_scan_path(result, outer_path, tuple_candidate);
}


static Selectivity
replay_index_cardinality(PlannerInfo *root, IndexPath *path, Selectivity native_selectivity)
{
	IndexOptInfo *index = path->indexinfo;

	if (previous_index_hook)
		native_selectivity = previous_index_hook(root, path, native_selectivity);
	if (binding_context == NULL)
		return native_selectivity;
	/*
	 * Bitmap index output counts candidates satisfying index quals, not the
	 * full base/parameterized relation. Keep its identity separate from rows.
	 * Exact btree quals admit measured replay; approximate AMs retain native
	 * selectivity and must not be treated as exact cardinality observations.
	 */
	path->index_cardinality_id_valid = false;
	path->index_preserve_fractional_rows = false;
	path->index_matches_exact = index->relam == BTREE_AM_OID;
	if (cardinality_estimate_hook != NULL)
	{
		CardinalityEstimateContext context;
		List *clauses = NIL;
		ListCell *lc;
		Cardinality matches;

		foreach(lc, path->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);

			clauses = list_concat(clauses, list_copy(iclause->indexquals));
			if (iclause->lossy)
				path->index_matches_exact = false;
		}
		foreach(lc, index->indpred)
			clauses = lappend(clauses,
				make_simple_restrictinfo(root, (Expr *) lfirst(lc)));
		MemSet(&context, 0, sizeof(context));
		context.kind = CARDINALITY_ESTIMATE_INDEX_MATCH;
		context.rel = path->path.parent;
		context.restrictlist = clauses;
		context.required_outer = PATH_REQ_OUTER(&path->path);
		context.driving_path = path->path.plan_replay_physical_driver;
		context.native_rows = clamp_row_est(native_selectivity * path->path.parent->tuples);
		matches = cardinality_estimate_hook(root, &context);
		path->index_preserve_fractional_rows =
			context.preserve_fractional_rows && path->index_matches_exact;
		path->index_cardinality_id_valid = context.cardinality_id_valid;
		path->index_cardinality_id_hi = context.cardinality_id_hi;
		path->index_cardinality_id_lo = context.cardinality_id_lo;
		if (path->index_matches_exact && path->path.parent->tuples > 0 &&
			matches != context.native_rows)
			native_selectivity = Min(1.0, Max(0.0, matches / path->path.parent->tuples));
		list_free(clauses);
	}

	return native_selectivity;
}


static void
replay_bitmap_index_plan(PlannerInfo *root, IndexPath *path, Plan *plan)
{
	if (previous_bitmap_hook)
		previous_bitmap_hook(root, path, plan);
	if (binding_context == NULL)
		return;
	plan->plan_replay_path_sequence_valid = path->path.plan_replay_path_sequence_valid;
	plan->plan_replay_path_sequence = path->path.plan_replay_path_sequence;
	plan->plan_replay_path_id_valid = path->path.plan_replay_path_id_valid;
	plan->plan_replay_path_id_hi = path->path.plan_replay_path_id_hi;
	plan->plan_replay_path_id_lo = path->path.plan_replay_path_id_lo;
	plan->plan_replay_cardinality_sequence_valid = false;
	plan->plan_replay_cardinality_id_valid = path->index_cardinality_id_valid;
	plan->plan_replay_cardinality_id_hi = path->index_cardinality_id_hi;
	plan->plan_replay_cardinality_id_lo = path->index_cardinality_id_lo;
	plan->plan_replay_row_scope_valid = path->index_cardinality_id_valid && path->index_matches_exact;
	plan->plan_replay_parallel_workers = 0;
	plan->plan_replay_leader_participation = parallel_leader_participation;
	plan->plan_rows = clamp_replayed_row_est(path->indexselectivity * path->path.parent->tuples,
										   path->index_preserve_fractional_rows);
}

static void
replay_explain_node(PlanState *state, List *ancestors, const char *relationship,
				   const char *plan_name, ExplainState *es)
{
	Plan *plan = state->plan;

	if (previous_explain_hook)
		previous_explain_hook(state, ancestors, relationship, plan_name, es);
	if (!IsA(plan, BitmapIndexScan))
		return;
	if (plan->plan_replay_path_id_valid && !plan->plan_replay_path_sequence_valid)
	{
		char id[33];

		snprintf(id, sizeof(id), "%016" PRIx64 "%016" PRIx64,
				 plan->plan_replay_path_id_hi, plan->plan_replay_path_id_lo);
		ExplainPropertyText("Plan Replay Path ID", id, es);
	}
	if (plan->plan_replay_cardinality_id_valid)
		ExplainPropertyText("Plan Replay Cardinality Scope", "index_candidates", es);
}

void
plan_replay_binding_begin(void)
{
	Assert(binding_context == NULL);
	binding_context = AllocSetContextCreate(CurrentMemoryContext,
										   "pg_plan_replay bound scan contexts",
										   ALLOCSET_DEFAULT_SIZES);
	bound_cache = NIL;
}

void
plan_replay_binding_end(void)
{
	if (binding_context)
		MemoryContextDelete(binding_context);
	binding_context = NULL;
	bound_cache = NIL;
}

void
plan_replay_binding_init(void)
{
	previous_bind_hook = parameterized_scan_bind_hook;
	parameterized_scan_bind_hook = plan_replay_bind_scan;
	previous_index_hook = index_cardinality_hook;
	index_cardinality_hook = replay_index_cardinality;
	previous_bitmap_hook = bitmap_index_plan_hook;
	bitmap_index_plan_hook = replay_bitmap_index_plan;
	previous_explain_hook = explain_per_node_hook;
	explain_per_node_hook = replay_explain_node;
}

void
plan_replay_binding_fini(void)
{
	if (parameterized_scan_bind_hook == plan_replay_bind_scan)
		parameterized_scan_bind_hook = previous_bind_hook;
	if (index_cardinality_hook == replay_index_cardinality)
		index_cardinality_hook = previous_index_hook;
	if (bitmap_index_plan_hook == replay_bitmap_index_plan)
		bitmap_index_plan_hook = previous_bitmap_hook;
	if (explain_per_node_hook == replay_explain_node)
		explain_per_node_hook = previous_explain_hook;
}
