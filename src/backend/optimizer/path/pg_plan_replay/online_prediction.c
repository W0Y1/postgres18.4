#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "portability/instr_time.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/lsyscache.h"

#include "model_client.h"
#include "online_prediction.h"

#define PLAN_REPLAY_MODEL_TIMEOUT_MS 60000
#define PLAN_REPLAY_MODEL_MAX_BATCH_SIZE 256

static char *model_socket_path = NULL;

static const char *
online_operator_name(Path *path)
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
		case T_Hash: return "Hash";
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
online_join_type_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER: return "Inner";
		case JOIN_LEFT: return "Left";
		case JOIN_FULL: return "Full";
		case JOIN_RIGHT: return "Right";
		case JOIN_SEMI: return "Semi";
		case JOIN_ANTI: return "Anti";
		case JOIN_RIGHT_SEMI: return "Right Semi";
		case JOIN_RIGHT_ANTI: return "Right Anti";
		default: return "Unknown";
	}
}

static void
online_append_relids(StringInfo output, Relids relids)
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
online_append_relation_names(StringInfo output, PlannerInfo *root, Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while (root != NULL && (member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(member, root);
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
online_append_relation_identities(StringInfo output, PlannerInfo *root,
								  Relids relids)
{
	int member = -1;
	bool first = true;

	appendStringInfoChar(output, '[');
	while (root != NULL && (member = bms_next_member(relids, member)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(member, root);
		char *relation;
		char *schema;

		if (rte == NULL || rte->rtekind != RTE_RELATION)
			continue;
		relation = get_rel_name(rte->relid);
		schema = get_namespace_name(get_rel_namespace(rte->relid));
		if (relation == NULL)
		{
			if (schema != NULL)
				pfree(schema);
			continue;
		}
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfo(output, "{\"rti\":%d,\"schema\":", member);
		if (schema != NULL)
			escape_json(output, schema);
		else
			appendStringInfoString(output, "null");
		appendStringInfoString(output, ",\"relation\":");
		escape_json(output, relation);
		appendStringInfoString(output, ",\"alias\":");
		if (rte->eref != NULL && rte->eref->aliasname != NULL)
			escape_json(output, rte->eref->aliasname);
		else
			appendStringInfoString(output, "null");
		appendStringInfoChar(output, '}');
		pfree(relation);
		if (schema != NULL)
			pfree(schema);
	}
	appendStringInfoChar(output, ']');
}

static Node *
online_strip_relabel(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

static bool
online_resolve_var(PlannerInfo *root, Var *var, char **relation, char **column)
{
	RangeTblEntry *rte;

	*relation = NULL;
	*column = NULL;
	if (root == NULL || root->parse == NULL || var->varlevelsup != 0 ||
		var->varno <= 0 || var->varno > list_length(root->parse->rtable) ||
		var->varattno <= 0)
		return false;
	rte = planner_rt_fetch(var->varno, root);
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;
	*relation = get_rel_name(rte->relid);
	*column = get_attname(rte->relid, var->varattno, true);
	if (*relation == NULL || *column == NULL)
	{
		if (*relation != NULL)
			pfree(*relation);
		if (*column != NULL)
			pfree(*column);
		*relation = NULL;
		*column = NULL;
		return false;
	}
	return true;
}

static void
online_append_var_origin(StringInfo output, PlannerInfo *root, Var *var,
						 const char *prefix)
{
	RangeTblEntry *rte;
	char *schema;

	if (root == NULL || root->parse == NULL || var->varlevelsup != 0 ||
		var->varno <= 0 || var->varno > list_length(root->parse->rtable))
		return;
	rte = planner_rt_fetch(var->varno, root);
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return;
	appendStringInfo(output, ",\"%sschema\":", prefix);
	schema = get_namespace_name(get_rel_namespace(rte->relid));
	if (schema != NULL)
	{
		escape_json(output, schema);
		pfree(schema);
	}
	else
		appendStringInfoString(output, "null");
	appendStringInfo(output, ",\"%srelation_alias\":", prefix);
	if (rte->eref != NULL && rte->eref->aliasname != NULL)
		escape_json(output, rte->eref->aliasname);
	else
		appendStringInfoString(output, "null");
}

static void
online_append_datum(StringInfo output, Oid type_oid, Datum value, bool isnull)
{
	Oid output_function;
	bool type_is_varlena;
	char *text;

	if (isnull)
	{
		appendStringInfoString(output, "null");
		return;
	}
	getTypeOutputInfo(type_oid, &output_function, &type_is_varlena);
	text = OidOutputFunctionCall(output_function, value);
	escape_json(output, text);
	pfree(text);
}

static bool
online_append_array_literal(StringInfo output, Node *node, Oid *element_type)
{
	if (IsA(node, ArrayExpr))
	{
		ArrayExpr *array = (ArrayExpr *) node;
		ListCell *cell;
		bool first = true;

		*element_type = array->element_typeid;
		appendStringInfoChar(output, '[');
		foreach(cell, array->elements)
		{
			Node *element = online_strip_relabel((Node *) lfirst(cell));
			Const *constant;

			if (!IsA(element, Const))
				return false;
			constant = (Const *) element;
			if (!first)
				appendStringInfoChar(output, ',');
			first = false;
			online_append_datum(output, constant->consttype,
							constant->constvalue, constant->constisnull);
		}
		appendStringInfoChar(output, ']');
		return true;
	}
	if (IsA(node, Const))
	{
		Const *constant = (Const *) node;
		ArrayType *array;
		Datum *values;
		bool *nulls;
		int count;
		int16 typlen;
		bool typbyval;
		char typalign;
		int index;

		if (constant->constisnull)
			return false;
		*element_type = get_element_type(constant->consttype);
		if (!OidIsValid(*element_type))
			return false;
		array = DatumGetArrayTypeP(constant->constvalue);
		get_typlenbyvalalign(*element_type, &typlen, &typbyval, &typalign);
		deconstruct_array(array, *element_type, typlen, typbyval, typalign,
						  &values, &nulls, &count);
		appendStringInfoChar(output, '[');
		for (index = 0; index < count; index++)
		{
			if (index > 0)
				appendStringInfoChar(output, ',');
			online_append_datum(output, *element_type, values[index], nulls[index]);
		}
		appendStringInfoChar(output, ']');
		pfree(values);
		pfree(nulls);
		return true;
	}
	return false;
}

static void
online_free_strings(List *strings)
{
	ListCell *cell;

	foreach(cell, strings)
		pfree(lfirst(cell));
	list_free(strings);
}

static List *
online_add_unique_string(List *strings, char *value)
{
	ListCell *cell;

	foreach(cell, strings)
	{
		if (strcmp((const char *) lfirst(cell), value) == 0)
		{
			pfree(value);
			return strings;
		}
	}
	return lappend(strings, value);
}

static void
online_append_string_array(StringInfo output, List *strings)
{
	ListCell *cell;
	bool first = true;

	appendStringInfoChar(output, '[');
	foreach(cell, strings)
	{
		if (!first)
			appendStringInfoChar(output, ',');
		first = false;
		appendStringInfoString(output, (const char *) lfirst(cell));
	}
	appendStringInfoChar(output, ']');
}

static bool online_append_filter_node_internal(StringInfo output,
											   PlannerInfo *root, Node *node,
											   bool negate);

static bool
online_append_boolean_filter(StringInfo output, PlannerInfo *root,
							 BoolExpr *expression, bool negate)
{
	List *children = NIL;
	ListCell *cell;
	const char *operator_name;
	bool require_all;

	if (expression->boolop == NOT_EXPR)
	{
		if (list_length(expression->args) != 1)
			return false;
		return online_append_filter_node_internal(output, root,
											  (Node *) linitial(expression->args),
											  !negate);
	}
	if ((expression->boolop == AND_EXPR) != negate)
		operator_name = "AND";
	else
		operator_name = "OR";
	require_all = strcmp(operator_name, "OR") == 0;
	foreach(cell, expression->args)
	{
		StringInfoData child;

		initStringInfo(&child);
		if (online_append_filter_node_internal(&child, root,
											   (Node *) lfirst(cell), negate))
			children = online_add_unique_string(children, child.data);
		else
		{
			pfree(child.data);
			if (require_all)
			{
				online_free_strings(children);
				return false;
			}
		}
	}
	if (children == NIL)
		return false;
	if (list_length(children) == 1)
		appendStringInfoString(output, (const char *) linitial(children));
	else
	{
		appendStringInfoString(output, "{\"operator\":");
		escape_json(output, operator_name);
		appendStringInfoString(output, ",\"children\":");
		online_append_string_array(output, children);
		appendStringInfoChar(output, '}');
	}
	online_free_strings(children);
	return true;
}

static bool
online_append_boolean_var_leaf(StringInfo output, PlannerInfo *root, Var *variable,
							   const char *operator_name,
							   const char *literal, bool isnull)
{
	char *relation;
	char *column;

	if (!online_resolve_var(root, variable, &relation, &column))
		return false;
	appendStringInfoString(output, "{\"operator\":");
	escape_json(output, operator_name);
	appendStringInfoString(output, ",\"relation\":");
	escape_json(output, relation);
	appendStringInfoString(output, ",\"column\":");
	escape_json(output, column);
	online_append_var_origin(output, root, variable, "");
	appendStringInfoString(output, ",\"literal\":");
	if (isnull)
		appendStringInfoString(output, "null");
	else
		escape_json(output, literal);
	appendStringInfo(output, ",\"literal_type_oid\":%u,\"children\":[]}",
					 variable->vartype);
	pfree(relation);
	pfree(column);
	return true;
}

static bool
online_append_boolean_test(StringInfo output, PlannerInfo *root,
						   BooleanTest *test, bool negate)
{
	BoolTestType test_type = test->booltesttype;
	Node *argument = online_strip_relabel((Node *) test->arg);
	Var *variable;
	StringInfoData left;
	StringInfoData right;
	const char *literal;

	if (!IsA(argument, Var))
		return false;
	variable = (Var *) argument;
	if (negate)
	{
		switch (test_type)
		{
			case IS_TRUE: test_type = IS_NOT_TRUE; break;
			case IS_NOT_TRUE: test_type = IS_TRUE; break;
			case IS_FALSE: test_type = IS_NOT_FALSE; break;
			case IS_NOT_FALSE: test_type = IS_FALSE; break;
			case IS_UNKNOWN: test_type = IS_NOT_UNKNOWN; break;
			case IS_NOT_UNKNOWN: test_type = IS_UNKNOWN; break;
		}
	}
	if (test_type == IS_UNKNOWN || test_type == IS_NOT_UNKNOWN)
		return online_append_boolean_var_leaf(output, root, variable,
										   test_type == IS_UNKNOWN ?
										   "IS NULL" : "IS NOT NULL",
										   NULL, true);
	if (test_type == IS_TRUE || test_type == IS_FALSE)
	{
		literal = test_type == IS_TRUE ? "true" : "false";
		return online_append_boolean_var_leaf(output, root, variable, "=",
										   literal, false);
	}

	/* IS NOT TRUE/FALSE includes NULL, so preserve it as an OR tree. */
	initStringInfo(&left);
	initStringInfo(&right);
	literal = test_type == IS_NOT_TRUE ? "false" : "true";
	if (!online_append_boolean_var_leaf(&left, root, variable, "=", literal,
										false) ||
		!online_append_boolean_var_leaf(&right, root, variable, "IS NULL", NULL,
										 true))
	{
		pfree(left.data);
		pfree(right.data);
		return false;
	}
	appendStringInfo(output, "{\"operator\":\"OR\",\"children\":[%s,%s]}",
					 left.data, right.data);
	pfree(left.data);
	pfree(right.data);
	return true;
}

static bool
online_append_filter_node_internal(StringInfo output, PlannerInfo *root,
								   Node *node, bool negate)
{
	if (node == NULL)
		return false;
	if (IsA(node, RestrictInfo))
		node = (Node *) ((RestrictInfo *) node)->clause;
	node = online_strip_relabel(node);
	if (IsA(node, BoolExpr))
		return online_append_boolean_filter(output, root, (BoolExpr *) node,
										negate);
	if (IsA(node, BooleanTest))
		return online_append_boolean_test(output, root, (BooleanTest *) node,
									  negate);
	if (IsA(node, NullTest))
	{
		NullTest *test = (NullTest *) node;
		Node *argument = online_strip_relabel((Node *) test->arg);
		char *relation;
		char *column;

		if (!IsA(argument, Var) ||
			!online_resolve_var(root, (Var *) argument, &relation, &column))
			return false;
		appendStringInfoString(output, "{\"operator\":");
		escape_json(output,
					(test->nulltesttype == IS_NULL) != negate ?
					"IS NULL" : "IS NOT NULL");
		appendStringInfoString(output, ",\"relation\":");
		escape_json(output, relation);
		appendStringInfoString(output, ",\"column\":");
		escape_json(output, column);
		online_append_var_origin(output, root, (Var *) argument, "");
		appendStringInfo(output, ",\"literal\":null,\"literal_type_oid\":%u,"
						 "\"children\":[]}", ((Var *) argument)->vartype);
		pfree(relation);
		pfree(column);
		return true;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *array_operator = (ScalarArrayOpExpr *) node;
		Node *left;
		Node *right;
		char *relation;
		char *column;
		char *operator_name;
		StringInfoData literal;
		Oid literal_type;

		if (negate || !array_operator->useOr ||
			list_length(array_operator->args) != 2)
			return false;
		operator_name = get_opname(array_operator->opno);
		if (operator_name == NULL || strcmp(operator_name, "=") != 0)
		{
			if (operator_name != NULL)
				pfree(operator_name);
			return false;
		}
		pfree(operator_name);
		left = online_strip_relabel((Node *) linitial(array_operator->args));
		right = online_strip_relabel((Node *) lsecond(array_operator->args));
		if (!IsA(left, Var) ||
			!online_resolve_var(root, (Var *) left, &relation, &column))
			return false;
		initStringInfo(&literal);
		if (!online_append_array_literal(&literal, right, &literal_type))
		{
			pfree(literal.data);
			pfree(relation);
			pfree(column);
			return false;
		}
		appendStringInfoString(output, "{\"operator\":\"IN\",\"relation\":");
		escape_json(output, relation);
		appendStringInfoString(output, ",\"column\":");
		escape_json(output, column);
		online_append_var_origin(output, root, (Var *) left, "");
		appendStringInfo(output, ",\"literal\":%s,\"literal_type_oid\":%u,"
						 "\"children\":[]}", literal.data, literal_type);
		pfree(literal.data);
		pfree(relation);
		pfree(column);
		return true;
	}
	if (IsA(node, OpExpr))
	{
		OpExpr *operator = (OpExpr *) node;
		Node *left;
		Node *right;
		Var *variable;
		Const *constant;
		Oid operator_oid = operator->opno;
		char *operator_name;
		char *relation;
		char *column;

		if (list_length(operator->args) != 2)
			return false;
		left = online_strip_relabel((Node *) linitial(operator->args));
		right = online_strip_relabel((Node *) lsecond(operator->args));
		if (IsA(left, Var) && IsA(right, Const))
		{
			variable = (Var *) left;
			constant = (Const *) right;
		}
		else if (IsA(left, Const) && IsA(right, Var))
		{
			operator_oid = get_commutator(operator_oid);
			if (!OidIsValid(operator_oid))
				return false;
			variable = (Var *) right;
			constant = (Const *) left;
		}
		else
			return false;
		if (!online_resolve_var(root, variable, &relation, &column))
			return false;
		if (negate)
		{
			operator_oid = get_negator(operator_oid);
			if (!OidIsValid(operator_oid))
			{
				pfree(relation);
				pfree(column);
				return false;
			}
		}
		operator_name = get_opname(operator_oid);
		if (operator_name == NULL)
		{
			pfree(relation);
			pfree(column);
			return false;
		}
		appendStringInfoString(output, "{\"operator\":");
		escape_json(output, operator_name);
		appendStringInfoString(output, ",\"relation\":");
		escape_json(output, relation);
		appendStringInfoString(output, ",\"column\":");
		escape_json(output, column);
		online_append_var_origin(output, root, variable, "");
		appendStringInfoString(output, ",\"literal\":");
		online_append_datum(output, constant->consttype,
						constant->constvalue, constant->constisnull);
		appendStringInfo(output, ",\"literal_type_oid\":%u,\"children\":[]}",
						 constant->consttype);
		pfree(operator_name);
		pfree(relation);
		pfree(column);
		return true;
	}
	return false;
}

static bool
online_append_filter_node(StringInfo output, PlannerInfo *root, Node *node)
{
	return online_append_filter_node_internal(output, root, node, false);
}

static char *
online_filter_json(PlannerInfo *root, Node *node)
{
	StringInfoData result;

	initStringInfo(&result);
	if (!online_append_filter_node(&result, root, node))
	{
		pfree(result.data);
		return NULL;
	}
	return result.data;
}

static char *
online_join_json(PlannerInfo *root, Node *node)
{
	OpExpr *operator;
	Node *left;
	Node *right;
	char *left_relation;
	char *left_column;
	char *right_relation;
	char *right_column;
	char *operator_name;
	StringInfoData result;

	if (node == NULL)
		return NULL;
	if (IsA(node, RestrictInfo))
		node = (Node *) ((RestrictInfo *) node)->clause;
	node = online_strip_relabel(node);
	if (!IsA(node, OpExpr))
		return NULL;
	operator = (OpExpr *) node;
	if (list_length(operator->args) != 2)
		return NULL;
	left = online_strip_relabel((Node *) linitial(operator->args));
	right = online_strip_relabel((Node *) lsecond(operator->args));
	if (!IsA(left, Var) || !IsA(right, Var) ||
		!online_resolve_var(root, (Var *) left, &left_relation, &left_column))
		return NULL;
	if (!online_resolve_var(root, (Var *) right, &right_relation, &right_column))
	{
		pfree(left_relation);
		pfree(left_column);
		return NULL;
	}
	operator_name = get_opname(operator->opno);
	if (operator_name == NULL)
	{
		pfree(left_relation);
		pfree(left_column);
		pfree(right_relation);
		pfree(right_column);
		return NULL;
	}
	initStringInfo(&result);
	appendStringInfoString(&result, "{\"left_relation\":");
	escape_json(&result, left_relation);
	appendStringInfoString(&result, ",\"left_column\":");
	escape_json(&result, left_column);
	online_append_var_origin(&result, root, (Var *) left, "left_");
	appendStringInfoString(&result, ",\"operator\":");
	escape_json(&result, operator_name);
	appendStringInfoString(&result, ",\"right_relation\":");
	escape_json(&result, right_relation);
	appendStringInfoString(&result, ",\"right_column\":");
	escape_json(&result, right_column);
	online_append_var_origin(&result, root, (Var *) right, "right_");
	appendStringInfoChar(&result, '}');
	pfree(operator_name);
	pfree(left_relation);
	pfree(left_column);
	pfree(right_relation);
	pfree(right_column);
	return result.data;
}

static List *
online_collect_conditions(PlannerInfo *root, List *clauses, List *conditions,
						  bool joins)
{
	ListCell *cell;

	foreach(cell, clauses)
	{
		Node *node = (Node *) lfirst(cell);
		char *condition = joins ? online_join_json(root, node) :
			online_filter_json(root, node);

		if (condition != NULL)
			conditions = online_add_unique_string(conditions, condition);
	}
	return conditions;
}

static void
online_append_path_conditions(StringInfo output, PlannerInfo *root, Path *path,
							  bool joins)
{
	List *conditions = NIL;

	if (path->parent != NULL)
		conditions = online_collect_conditions(root,
										 path->parent->baserestrictinfo,
										 conditions, joins);
	if (path->param_info != NULL)
		conditions = online_collect_conditions(root, path->param_info->ppi_clauses,
										 conditions, joins);
	if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath))
		conditions = online_collect_conditions(root,
										 ((JoinPath *) path)->joinrestrictinfo,
										 conditions, joins);
	online_append_string_array(output, conditions);
	online_free_strings(conditions);
}

static const char *
online_aggregation_name(Node *expression)
{
	char *function_name;
	const char *result = "None";

	expression = online_strip_relabel(expression);
	if (!IsA(expression, Aggref))
		return result;
	function_name = get_func_name(((Aggref *) expression)->aggfnoid);
	if (function_name == NULL)
		return result;
	if (pg_strcasecmp(function_name, "count") == 0)
		result = "COUNT";
	else if (pg_strcasecmp(function_name, "sum") == 0)
		result = "SUM";
	else if (pg_strcasecmp(function_name, "min") == 0)
		result = "MIN";
	else if (pg_strcasecmp(function_name, "max") == 0)
		result = "MAX";
	else if (pg_strcasecmp(function_name, "avg") == 0)
		result = "AVG";
	pfree(function_name);
	return result;
}

static void
online_append_output_columns(StringInfo output, PlannerInfo *root,
							 PathTarget *target)
{
	ListCell *expression_cell;
	bool first_expression = true;

	appendStringInfoChar(output, '[');
	if (target == NULL)
	{
		appendStringInfoChar(output, ']');
		return;
	}
	foreach(expression_cell, target->exprs)
	{
		Node *expression = (Node *) lfirst(expression_cell);
		List *variables = pull_var_clause(expression,
										 PVC_RECURSE_AGGREGATES |
										 PVC_RECURSE_WINDOWFUNCS |
										 PVC_RECURSE_PLACEHOLDERS);
		List *columns = NIL;
		ListCell *variable_cell;
		const char *aggregation = online_aggregation_name(expression);

		foreach(variable_cell, variables)
		{
			Var *variable = lfirst_node(Var, variable_cell);
			char *relation;
			char *column;
			StringInfoData item;

			if (!online_resolve_var(root, variable, &relation, &column))
				continue;
			initStringInfo(&item);
			appendStringInfoString(&item, "{\"relation\":");
			escape_json(&item, relation);
			appendStringInfoString(&item, ",\"column\":");
			escape_json(&item, column);
			online_append_var_origin(&item, root, variable, "");
			appendStringInfoChar(&item, '}');
			columns = online_add_unique_string(columns, item.data);
			pfree(relation);
			pfree(column);
		}
		list_free(variables);
		if (columns != NIL || strcmp(aggregation, "None") != 0)
		{
			if (!first_expression)
				appendStringInfoChar(output, ',');
			first_expression = false;
			appendStringInfoString(output, "{\"aggregation\":");
			escape_json(output, aggregation);
			appendStringInfoString(output, ",\"columns\":");
			online_append_string_array(output, columns);
			appendStringInfoChar(output, '}');
		}
		online_free_strings(columns);
	}
	appendStringInfoChar(output, ']');
}

static bool
online_add_sort_expression(List **keys, PlannerInfo *root, Node *expression,
						   PathKey *pathkey)
{
	List *variables = pull_var_clause(expression,
								 PVC_RECURSE_AGGREGATES |
								 PVC_RECURSE_WINDOWFUNCS |
								 PVC_RECURSE_PLACEHOLDERS);
	ListCell *variable_cell;
	bool added = false;

	foreach(variable_cell, variables)
	{
		Var *variable = lfirst_node(Var, variable_cell);
		char *relation;
		char *column;
		StringInfoData item;

		if (!online_resolve_var(root, variable, &relation, &column))
			continue;
		initStringInfo(&item);
		appendStringInfoString(&item, "{\"relation\":");
		escape_json(&item, relation);
		appendStringInfoString(&item, ",\"column\":");
		escape_json(&item, column);
		online_append_var_origin(&item, root, variable, "");
		appendStringInfoString(&item, ",\"direction\":");
		escape_json(&item,
					pathkey->pk_cmptype == COMPARE_GT ? "DESC" : "ASC");
		appendStringInfo(&item,
					 ",\"nulls_first\":%s,\"collation_oid\":%u,"
					 "\"opfamily_oid\":%u",
					 pathkey->pk_nulls_first ? "true" : "false",
					 pathkey->pk_eclass->ec_collation, pathkey->pk_opfamily);
		appendStringInfoChar(&item, '}');
		*keys = online_add_unique_string(*keys, item.data);
		pfree(relation);
		pfree(column);
		added = true;
		break;
	}
	list_free(variables);
	return added;
}

static void
online_append_sort_keys(StringInfo output, PlannerInfo *root, Path *path)
{
	ListCell *pathkey_cell;
	List *keys = NIL;

	if ((path->pathtype != T_Sort && path->pathtype != T_IncrementalSort) ||
		path->pathtarget == NULL)
	{
		appendStringInfoString(output, "[]");
		return;
	}
	foreach(pathkey_cell, path->pathkeys)
	{
		PathKey *pathkey = lfirst_node(PathKey, pathkey_cell);
		EquivalenceClass *eclass = pathkey->pk_eclass;
		ListCell *cell;
		bool added = false;

		/* Prefer the exact ORDER/GROUP BY target selected by this PathKey. */
		if (eclass->ec_sortref != 0 && root != NULL)
		{
			foreach(cell, root->processed_tlist)
			{
				TargetEntry *entry = lfirst_node(TargetEntry, cell);

				if (entry->ressortgroupref == eclass->ec_sortref)
				{
					added = online_add_sort_expression(&keys, root,
													 (Node *) entry->expr,
													 pathkey);
					break;
				}
			}
		}
		if (added)
			continue;

		/* Non-query PathKeys have no sortref; use a relevant EC member. */
		foreach(cell, eclass->ec_members)
		{
			EquivalenceMember *member = lfirst_node(EquivalenceMember, cell);

			if (member->em_is_const ||
				(path->parent != NULL &&
				 !bms_is_empty(path->parent->relids) &&
				 !bms_is_subset(member->em_relids, path->parent->relids)))
				continue;
			if (online_add_sort_expression(&keys, root,
									   (Node *) member->em_expr, pathkey))
				break;
		}
	}
	online_append_string_array(output, keys);
	online_free_strings(keys);
}

static const char *
online_scan_direction_name(ScanDirection direction)
{
	switch (direction)
	{
		case ForwardScanDirection: return "Forward";
		case BackwardScanDirection: return "Backward";
		case NoMovementScanDirection: return "NoMovement";
	}
	return "Forward";
}

static const char *
online_aggregate_strategy_name(AggStrategy strategy)
{
	switch (strategy)
	{
		case AGG_PLAIN: return "Plain";
		case AGG_SORTED: return "Sorted";
		case AGG_HASHED: return "Hashed";
		case AGG_MIXED: return "Mixed";
	}
	return "Plain";
}

static const char *
online_partial_mode_name(AggSplit split)
{
	if (DO_AGGSPLIT_SKIPFINAL(split))
		return "Partial";
	if (DO_AGGSPLIT_COMBINE(split))
		return "Finalize";
	return "Simple";
}

static void online_append_path_json(StringInfo output, PlannerInfo *root,
									Path *path);

static void
online_append_child(StringInfo output, PlannerInfo *root, Path *child,
					bool *first)
{
	if (child == NULL)
		return;
	if (!*first)
		appendStringInfoChar(output, ',');
	*first = false;
	online_append_path_json(output, root, child);
}

static void
online_append_path_list(StringInfo output, PlannerInfo *root, List *paths,
						bool *first)
{
	ListCell *cell;

	foreach(cell, paths)
		online_append_child(output, root, (Path *) lfirst(cell), first);
}

static void
online_append_path_json(StringInfo output, PlannerInfo *root, Path *path)
{
	bool first_child = true;

	appendStringInfoChar(output, '{');
	appendStringInfoString(output, "\"op_name\":");
	escape_json(output, online_operator_name(path));
	appendStringInfo(output,
					 ",\"path_tag\":%d,\"plan_tag\":%d"
					 ",\"startup_cost\":%.17g,\"native_total_cost\":%.17g"
					 ",\"rows\":%.17g,\"width\":%d"
					 ",\"parallel_aware\":%s,\"parallel_safe\":%s"
					 ",\"parallel_workers\":%d,\"pathkeys\":%d,\"relids\":",
					 (int) nodeTag(path), (int) path->pathtype,
					 path->startup_cost, path->total_cost, path->rows,
					 path->pathtarget != NULL ? path->pathtarget->width : 0,
					 path->parallel_aware ? "true" : "false",
					 path->parallel_safe ? "true" : "false",
					 path->parallel_workers, list_length(path->pathkeys));
	online_append_relids(output, path->parent != NULL ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relation_names\":");
	online_append_relation_names(output, root,
								 path->parent != NULL ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"relation_identities\":");
	online_append_relation_identities(output, root,
									path->parent != NULL ? path->parent->relids : NULL);
	appendStringInfoString(output, ",\"required_outer\":");
	online_append_relids(output, PATH_REQ_OUTER(path));
	appendStringInfoString(output, ",\"filter_conditions\":");
	online_append_path_conditions(output, root, path, false);
	appendStringInfoString(output, ",\"join_conditions\":");
	online_append_path_conditions(output, root, path, true);
	appendStringInfoString(output, ",\"output_columns\":");
	online_append_output_columns(output, root, path->pathtarget);
	appendStringInfoString(output, ",\"sort_keys\":");
	online_append_sort_keys(output, root, path);
	if (IsA(path, IndexPath) && ((IndexPath *) path)->indexinfo != NULL)
	{
		char *index_name = get_rel_name(((IndexPath *) path)->indexinfo->indexoid);

		if (index_name != NULL)
		{
			appendStringInfoString(output, ",\"index_name\":");
			escape_json(output, index_name);
			pfree(index_name);
		}
		appendStringInfoString(output, ",\"scan_direction\":");
		escape_json(output,
					online_scan_direction_name(((IndexPath *) path)->indexscandir));
	}
	if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath))
	{
		appendStringInfoString(output, ",\"join_type\":");
		escape_json(output, online_join_type_name(((JoinPath *) path)->jointype));
	}
	if (IsA(path, AggPath))
	{
		AggPath *aggregate = (AggPath *) path;

		appendStringInfoString(output, ",\"aggregate_strategy\":");
		escape_json(output,
					online_aggregate_strategy_name(aggregate->aggstrategy));
		appendStringInfoString(output, ",\"partial_mode\":");
		escape_json(output, online_partial_mode_name(aggregate->aggsplit));
	}
	appendStringInfoString(output, ",\"children\":[");
	switch (nodeTag(path))
	{
		case T_BitmapHeapPath:
			online_append_child(output, root, ((BitmapHeapPath *) path)->bitmapqual, &first_child);
			break;
		case T_BitmapAndPath:
			online_append_path_list(output, root, ((BitmapAndPath *) path)->bitmapquals, &first_child);
			break;
		case T_BitmapOrPath:
			online_append_path_list(output, root, ((BitmapOrPath *) path)->bitmapquals, &first_child);
			break;
		case T_SubqueryScanPath:
			online_append_child(output, root, ((SubqueryScanPath *) path)->subpath, &first_child);
			break;
		case T_ForeignPath:
			online_append_child(output, root, ((ForeignPath *) path)->fdw_outerpath, &first_child);
			break;
		case T_CustomPath:
			online_append_path_list(output, root, ((CustomPath *) path)->custom_paths, &first_child);
			break;
		case T_AppendPath:
			online_append_path_list(output, root, ((AppendPath *) path)->subpaths, &first_child);
			break;
		case T_MergeAppendPath:
			online_append_path_list(output, root, ((MergeAppendPath *) path)->subpaths, &first_child);
			break;
		case T_MaterialPath:
			online_append_child(output, root, ((MaterialPath *) path)->subpath, &first_child);
			break;
		case T_MemoizePath:
			online_append_child(output, root, ((MemoizePath *) path)->subpath, &first_child);
			break;
		case T_UniquePath:
			online_append_child(output, root, ((UniquePath *) path)->subpath, &first_child);
			break;
		case T_GatherPath:
			online_append_child(output, root, ((GatherPath *) path)->subpath, &first_child);
			break;
		case T_GatherMergePath:
			online_append_child(output, root, ((GatherMergePath *) path)->subpath, &first_child);
			break;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			online_append_child(output, root, ((JoinPath *) path)->outerjoinpath, &first_child);
			online_append_child(output, root, ((JoinPath *) path)->innerjoinpath, &first_child);
			break;
		case T_ProjectionPath:
			online_append_child(output, root, ((ProjectionPath *) path)->subpath, &first_child);
			break;
		case T_ProjectSetPath:
			online_append_child(output, root, ((ProjectSetPath *) path)->subpath, &first_child);
			break;
		case T_SortPath:
		case T_IncrementalSortPath:
			online_append_child(output, root, ((SortPath *) path)->subpath, &first_child);
			break;
		case T_GroupPath:
			online_append_child(output, root, ((GroupPath *) path)->subpath, &first_child);
			break;
		case T_UpperUniquePath:
			online_append_child(output, root, ((UpperUniquePath *) path)->subpath, &first_child);
			break;
		case T_AggPath:
			online_append_child(output, root, ((AggPath *) path)->subpath, &first_child);
			break;
		case T_GroupingSetsPath:
			online_append_child(output, root, ((GroupingSetsPath *) path)->subpath, &first_child);
			break;
		case T_WindowAggPath:
			online_append_child(output, root, ((WindowAggPath *) path)->subpath, &first_child);
			break;
		case T_SetOpPath:
			online_append_child(output, root, ((SetOpPath *) path)->leftpath, &first_child);
			online_append_child(output, root, ((SetOpPath *) path)->rightpath, &first_child);
			break;
		case T_RecursiveUnionPath:
			online_append_child(output, root, ((RecursiveUnionPath *) path)->leftpath, &first_child);
			online_append_child(output, root, ((RecursiveUnionPath *) path)->rightpath, &first_child);
			break;
		case T_LockRowsPath:
			online_append_child(output, root, ((LockRowsPath *) path)->subpath, &first_child);
			break;
		case T_ModifyTablePath:
			online_append_child(output, root, ((ModifyTablePath *) path)->subpath, &first_child);
			break;
		case T_LimitPath:
			online_append_child(output, root, ((LimitPath *) path)->subpath, &first_child);
			break;
		default:
			break;
	}
	appendStringInfoString(output, "]}");
}

void
plan_replay_online_init(void)
{
	DefineCustomStringVariable("plan_replay.model_socket_path",
						   "Unix socket used by online Path cost prediction.",
						   NULL,
						   &model_socket_path,
						   "/tmp/pg_plan_replay.sock",
						   PGC_USERSET,
						   0,
						   NULL, NULL, NULL);
}

void
plan_replay_online_reset_stats(PlanReplayOnlineStats *stats)
{
	MemSet(stats, 0, sizeof(*stats));
}

bool
plan_replay_online_predict_paths(PlannerInfo *root, RelOptInfo *rel,
								 List *paths, bool partial,
								 PlanReplayOnlineStats *stats)
{
	int path_count = list_length(paths);
	Path **path_array;
	Cost *predictions;
	ListCell *cell;
	int offset;
	int index = 0;
	char *failure = NULL;
	instr_time started;
	instr_time elapsed;

	if (path_count == 0)
		return true;
	if (model_socket_path == NULL || model_socket_path[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_plan_replay model socket path is not configured")));
	path_array = palloc(sizeof(Path *) * path_count);
	predictions = palloc(sizeof(Cost) * path_count);
	foreach(cell, paths)
		path_array[index++] = (Path *) lfirst(cell);

	INSTR_TIME_SET_CURRENT(started);
	for (offset = 0; offset < path_count; offset += PLAN_REPLAY_MODEL_MAX_BATCH_SIZE)
	{
		StringInfoData request;
		int batch_count = Min(PLAN_REPLAY_MODEL_MAX_BATCH_SIZE,
							  path_count - offset);
		int batch_index;

		initStringInfo(&request);
		appendStringInfo(&request,
						 "{\"version\":1,\"type\":\"predict_cost\","
						 "\"partial\":%s,\"relids\":",
						 partial ? "true" : "false");
		online_append_relids(&request, rel->relids);
		appendStringInfoString(&request, ",\"paths\":[");
		for (batch_index = 0; batch_index < batch_count; batch_index++)
		{
			if (batch_index > 0)
				appendStringInfoChar(&request, ',');
			online_append_path_json(&request, root,
								path_array[offset + batch_index]);
		}
		appendStringInfoString(&request, "]}\n");
		stats->batches++;
		stats->paths += batch_count;
		if (!plan_replay_model_predict(model_socket_path,
									   PLAN_REPLAY_MODEL_TIMEOUT_MS,
									   request.data, request.len,
									   batch_count,
									   predictions + offset,
									   &failure))
		{
			pfree(request.data);
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_plan_replay model cost prediction failed"),
					 errdetail("%s", failure != NULL ? failure : "unknown model service error")));
		}
		pfree(request.data);
	}
	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	stats->inference_ms += INSTR_TIME_GET_MILLISEC(elapsed);

	for (index = 0; index < path_count; index++)
	{
		Path *path = path_array[index];

		path->total_cost = predictions[index];
		if (path->startup_cost > path->total_cost)
			path->startup_cost = path->total_cost;
	}
	stats->applied += path_count;
	if (failure != NULL)
		pfree(failure);
	pfree(predictions);
	pfree(path_array);
	return true;
}
