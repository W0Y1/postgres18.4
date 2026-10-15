/* 逻辑基数状态的规范化、128 位身份计算以及语句级缓存。 */
#include "postgres.h"

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "optimizer/cost.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "cardinality_identity.h"

#define CARDINALITY_ID_BYTES 16

/*
 * cardinality_id 由两层规范化对象组成：
 *
 * 1. token 表示一个不可再分的语义单元，例如关系、条件、EC、外连接边界；
 * 2. state 保存一个中间关系累计到当前为止的关系 token 和条件 token 集合。
 *
 * state 中的两个数组始终按 128 位 token 排序并去重。因此 INNER JOIN 只做
 * 集合并集，其 ID 不依赖左右输入、Join Path 类型或连接树的结合次序。需要方向
 * 语义的外连接和 Semi/Anti Join 则额外加入 join_boundary token。
 */
typedef struct CardinalityTokenEntry
{
	PlanReplayCardinalityId id;
	/* 保留完整规范串，用来在同一语句内检查 128 位截断哈希碰撞。 */
	char	   *descriptor;
} CardinalityTokenEntry;

typedef struct CardinalityStateEntry
{
	PlanReplayCardinalityId id;
	char	   *descriptor;
	CardinalityEstimateKind kind;
	int			nrelations;
	PlanReplayCardinalityId *relations;
	int			nconditions;
	PlanReplayCardinalityId *conditions;
} CardinalityStateEntry;

typedef struct ClauseCacheKey
{
	PlannerInfo *root;
	int			serial;
} ClauseCacheKey;

typedef struct ClauseCacheEntry
{
	ClauseCacheKey key;
	PlanReplayCardinalityId id;
} ClauseCacheEntry;

typedef enum CardinalityConditionRole
{
	CARDINALITY_CONDITION_FILTER,
	CARDINALITY_CONDITION_PARAMETER,
	CARDINALITY_CONDITION_JOINQUAL,
	CARDINALITY_CONDITION_QUAL
} CardinalityConditionRole;

typedef struct ConditionCacheKey
{
	PlannerInfo *root;
	const void *object;
	int			serial;
	CardinalityConditionRole role;
} ConditionCacheKey;

typedef struct ConditionCacheEntry
{
	ConditionCacheKey key;
	PlanReplayCardinalityId id;
} ConditionCacheEntry;

typedef struct ObjectCacheKey
{
	PlannerInfo *root;
	const void *object;
} ObjectCacheKey;

typedef struct ObjectCacheEntry
{
	ObjectCacheKey key;
	PlanReplayCardinalityId id;
} ObjectCacheEntry;

typedef struct RelationCacheKey
{
	PlannerInfo *root;
	Index		rti;
} RelationCacheKey;

typedef struct RelationCacheEntry
{
	RelationCacheKey key;
	PlanReplayCardinalityId id;
} RelationCacheEntry;

typedef struct ParamWalkerContext
{
	PlannerInfo *root;
	StringInfo	output;
} ParamWalkerContext;

static HTAB *cardinality_token_table;
static HTAB *cardinality_state_table;
static HTAB *clause_cache;
static HTAB *condition_cache;
static HTAB *eclass_cache;
static HTAB *relation_cache;
static bool cardinality_replay_mode;
static MemoryContext cardinality_identity_context;

static int
cardinality_id_compare(const void *left, const void *right)
{
	const PlanReplayCardinalityId *a = left;
	const PlanReplayCardinalityId *b = right;

	if (a->hi < b->hi)
		return -1;
	if (a->hi > b->hi)
		return 1;
	if (a->lo < b->lo)
		return -1;
	if (a->lo > b->lo)
		return 1;
	return 0;
}

static int
string_pointer_compare(const void *left, const void *right)
{
	return strcmp(*(const char *const *) left, *(const char *const *) right);
}

static bool
cardinality_id_equal(PlanReplayCardinalityId left,
					 PlanReplayCardinalityId right)
{
	return left.hi == right.hi && left.lo == right.lo;
}

static void
hash_uint32(pg_cryptohash_ctx *hash, uint32 value)
{
	uint8 bytes[4];

	bytes[0] = (value >> 24) & 0xff;
	bytes[1] = (value >> 16) & 0xff;
	bytes[2] = (value >> 8) & 0xff;
	bytes[3] = value & 0xff;
	if (pg_cryptohash_update(hash, bytes, sizeof(bytes)) < 0)
		elog(ERROR, "could not update cardinality SHA-256 context");
}

static void
hash_uint64(pg_cryptohash_ctx *hash, uint64 value)
{
	uint8 bytes[8];
	int			index;

	for (index = 7; index >= 0; index--)
	{
		bytes[index] = value & 0xff;
		value >>= 8;
	}
	if (pg_cryptohash_update(hash, bytes, sizeof(bytes)) < 0)
		elog(ERROR, "could not update cardinality SHA-256 context");
}

static pg_cryptohash_ctx *
begin_hash(const char *version)
{
	pg_cryptohash_ctx *hash = pg_cryptohash_create(PG_SHA256);

	if (hash == NULL || pg_cryptohash_init(hash) < 0 ||
		pg_cryptohash_update(hash, (const uint8 *) version, strlen(version)) < 0)
		elog(ERROR, "could not initialize cardinality SHA-256 context");
	return hash;
}

static PlanReplayCardinalityId
finish_hash(pg_cryptohash_ctx *hash)
{
	uint8 digest[PG_SHA256_DIGEST_LENGTH];
	PlanReplayCardinalityId id = {0, 0};
	int			index;

	if (pg_cryptohash_final(hash, digest, sizeof(digest)) < 0)
	{
		char	   *message = pstrdup(pg_cryptohash_error(hash));

		pg_cryptohash_free(hash);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not hash cardinality descriptor: %s", message)));
	}
	pg_cryptohash_free(hash);
	for (index = 0; index < 8; index++)
		id.hi = (id.hi << 8) | digest[index];
	for (index = 8; index < CARDINALITY_ID_BYTES; index++)
		id.lo = (id.lo << 8) | digest[index];
	return id;
}

static PlanReplayCardinalityId
hash_descriptor(const char *version, const char *descriptor)
{
	pg_cryptohash_ctx *hash = begin_hash(version);

	hash_uint64(hash, strlen(descriptor));
	if (pg_cryptohash_update(hash, (const uint8 *) descriptor,
							 strlen(descriptor)) < 0)
		elog(ERROR, "could not update cardinality SHA-256 context");
	return finish_hash(hash);
}

void
plan_replay_format_cardinality_id(PlanReplayCardinalityId id, char output[33])
{
	snprintf(output, 33, "%016" PRIx64 "%016" PRIx64, id.hi, id.lo);
}

static void
register_token(PlanReplayCardinalityId id, const char *descriptor)
{
	CardinalityTokenEntry *entry;
	bool		found;

	entry = hash_search(cardinality_token_table, &id, HASH_ENTER, &found);
	if (!found)
	{
		MemoryContext old_context;

		old_context = MemoryContextSwitchTo(cardinality_identity_context);
		entry->descriptor = pstrdup(descriptor);
		MemoryContextSwitchTo(old_context);
		return;
	}
	if (strcmp(entry->descriptor, descriptor) != 0)
	{
		char text[33];

		plan_replay_format_cardinality_id(id, text);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cardinality token collision for %s", text),
				 errdetail("Two different canonical tokens produced the same 128-bit ID.")));
	}
}

static PlanReplayCardinalityId
make_token(const char *descriptor)
{
	PlanReplayCardinalityId id =
		hash_descriptor("pg_plan_replay_cardinality_token_v1", descriptor);

	register_token(id, descriptor);
	return id;
}

static bool
append_bound_param_walker(Node *node, ParamWalkerContext *context)
{
	Param *param;
	ParamListInfo params;
	ParamExternData workspace;
	ParamExternData *prm = NULL;

	if (node == NULL)
		return false;
	if (!IsA(node, Param))
	{
		if (IsA(node, Query))
			return query_tree_walker((Query *) node,
									 append_bound_param_walker, context, 0);
		return expression_tree_walker(node, append_bound_param_walker,
								  context);
	}

	param = (Param *) node;
	if (param->paramkind != PARAM_EXTERN)
		return false;

	/*
	 * nodeToString() 只记录 PARAM_EXTERN 的编号和声明类型，不包含本次规划实际
	 * 绑定的 Datum。若不追加类型、NULL 状态和值，不同参数值可能错误命中同一
	 * 个回放 ID。capture 模式允许把未绑定状态记入 token；replay 模式则拒绝
	 * 这种无法唯一定位实际基数的输入。
	 */
	params = context->root->glob->boundParams;
	if (params != NULL && param->paramid > 0 &&
		param->paramid <= params->numParams)
	{
		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, param->paramid, true, &workspace);
		else
			prm = &params->params[param->paramid - 1];
	}
	if (prm == NULL || !OidIsValid(prm->ptype))
	{
		if (cardinality_replay_mode)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot build cardinality ID without a value for external parameter $%d",
							param->paramid),
					 errdetail("Sparse cardinality replay refuses to match an unbound PARAM_EXTERN.")));
		appendStringInfo(context->output, "|extern=%d:%u:unbound",
						 param->paramid, param->paramtype);
		return false;
	}

	appendStringInfo(context->output, "|extern=%d:%u:%u:",
					 param->paramid, param->paramtype, prm->ptype);
	if (prm->isnull)
		appendStringInfoString(context->output, "null");
	else
	{
		Oid			output_function;
		bool		type_is_varlena;
		char	   *value;

		getTypeOutputInfo(prm->ptype, &output_function, &type_is_varlena);
		value = OidOutputFunctionCall(output_function, prm->value);
		appendStringInfo(context->output, "%zu:", strlen(value));
		appendStringInfoString(context->output, value);
		pfree(value);
	}
	return false;
}

static char *
node_descriptor(PlannerInfo *root, const Node *node)
{
	StringInfoData output;
	ParamWalkerContext context;
	char	   *dump = nodeToString(node);

	/* 长度前缀避免相邻字段在串联时产生边界歧义。 */
	initStringInfo(&output);
	appendStringInfo(&output, "level=%d|node=%zu:", root->query_level,
					 strlen(dump));
	appendStringInfoString(&output, dump);
	pfree(dump);
	context.root = root;
	context.output = &output;
	append_bound_param_walker((Node *) node, &context);
	return output.data;
}

static EquivalenceClass *
canonical_eclass(EquivalenceClass *ec)
{
	while (ec != NULL && ec->ec_merged != NULL)
		ec = ec->ec_merged;
	return ec;
}

static PlanReplayCardinalityId
eclass_token(PlannerInfo *root, EquivalenceClass *ec)
{
	ObjectCacheKey key;
	ObjectCacheEntry *cache_entry;
	StringInfoData descriptor;
	char	  **members;
	ListCell   *cell;
	int			count;
	int			index = 0;
	bool		found;
	PlanReplayCardinalityId id;

	/*
	 * 同一个 EquivalenceClass 可在不同连接拆分下生成不同的等式表达式。这里
	 * 统一追到合并后的 EC，并按成员规范串排序，使这些推导等式共享一个语义
	 * token，而不是把偶然选中的 RestrictInfo 表达式写进身份。
	 */
	ec = canonical_eclass(ec);
	MemSet(&key, 0, sizeof(key));
	key.root = root;
	key.object = ec;
	cache_entry = hash_search(eclass_cache, &key, HASH_FIND, NULL);
	if (cache_entry != NULL)
		return cache_entry->id;

	count = list_length(ec->ec_members);
	members = palloc(sizeof(char *) * Max(count, 1));
	foreach(cell, ec->ec_members)
	{
		EquivalenceMember *member = lfirst_node(EquivalenceMember, cell);
		char	   *expression = node_descriptor(root, (Node *) member->em_expr);
		StringInfoData value;

		initStringInfo(&value);
		appendStringInfo(&value, "datatype=%u|expression=%zu:%s",
						 member->em_datatype, strlen(expression), expression);
		pfree(expression);
		members[index++] = value.data;
	}
	qsort(members, count, sizeof(char *), string_pointer_compare);
	initStringInfo(&descriptor);
	appendStringInfo(&descriptor, "ec|level=%d|collation=%u|opfamilies=",
					 root->query_level, ec->ec_collation);
	{
		char	   *opfamilies = nodeToString(ec->ec_opfamilies);

		appendStringInfo(&descriptor, "%zu:%s|members=[", strlen(opfamilies),
						 opfamilies);
		pfree(opfamilies);
	}
	for (index = 0; index < count; index++)
	{
		appendStringInfo(&descriptor, "%zu:%s;", strlen(members[index]),
						 members[index]);
		pfree(members[index]);
	}
	appendStringInfoChar(&descriptor, ']');
	pfree(members);
	id = make_token(descriptor.data);
	pfree(descriptor.data);
	cache_entry = hash_search(eclass_cache, &key, HASH_ENTER, &found);
	Assert(!found);
	cache_entry->id = id;
	return id;
}

static PlanReplayCardinalityId
expression_token(PlannerInfo *root, Node *expression)
{
	char	   *descriptor = node_descriptor(root, expression);
	PlanReplayCardinalityId id = make_token(descriptor);

	pfree(descriptor);
	return id;
}

static PlanReplayCardinalityId
clause_semantic_token(PlannerInfo *root, RestrictInfo *rinfo)
{
	ClauseCacheKey key;
	ClauseCacheEntry *cache_entry;
	EquivalenceClass *left;
	EquivalenceClass *right;
	PlanReplayCardinalityId id;
	bool		found;

	MemSet(&key, 0, sizeof(key));
	key.root = root;
	key.serial = rinfo->rinfo_serial;
	if (key.serial > 0)
	{
		cache_entry = hash_search(clause_cache, &key, HASH_FIND, NULL);
		if (cache_entry != NULL)
			return cache_entry->id;
	}

	left = canonical_eclass(rinfo->left_ec);
	right = canonical_eclass(rinfo->right_ec);
	if (rinfo->parent_ec != NULL)
		id = eclass_token(root, rinfo->parent_ec);
	else if (left != NULL && left == right)
		id = eclass_token(root, left);
	else
		id = expression_token(root, (Node *) rinfo->clause);

	if (key.serial > 0)
	{
		cache_entry = hash_search(clause_cache, &key, HASH_ENTER, &found);
		Assert(!found);
		cache_entry->id = id;
	}
	return id;
}

static PlanReplayCardinalityId
condition_token(PlannerInfo *root, RestrictInfo *rinfo,
				CardinalityConditionRole role)
{
	ConditionCacheKey key;
	ConditionCacheEntry *cache_entry;
	PlanReplayCardinalityId semantic;
	StringInfoData descriptor;
	char text[33];
	PlanReplayCardinalityId id;
	const char *role_name;
	bool		found;

	MemSet(&key, 0, sizeof(key));
	key.root = root;
	key.serial = rinfo->rinfo_serial;
	key.object = key.serial > 0 ? NULL : rinfo;
	key.role = role;
	cache_entry = hash_search(condition_cache, &key, HASH_FIND, NULL);
	if (cache_entry != NULL)
		return cache_entry->id;

	switch (role)
	{
		case CARDINALITY_CONDITION_FILTER:
			role_name = "filter";
			break;
		case CARDINALITY_CONDITION_PARAMETER:
			role_name = "parameter";
			break;
		case CARDINALITY_CONDITION_JOINQUAL:
			role_name = "joinqual";
			break;
		case CARDINALITY_CONDITION_QUAL:
			role_name = "qual";
			break;
		default:
			elog(ERROR, "unknown cardinality condition role: %d", (int) role);
	}

	/*
	 * 相同谓词处于 ON、下推 filter 或参数条件时语义可能不同，所以在表达式
	 * token 外再包一层 role。rinfo_serial 只作为本轮缓存键，从不进入 token。
	 */
	semantic = clause_semantic_token(root, rinfo);
	plan_replay_format_cardinality_id(semantic, text);
	initStringInfo(&descriptor);
	appendStringInfo(&descriptor, "condition|role=%s|semantic=%s", role_name, text);
	id = make_token(descriptor.data);
	pfree(descriptor.data);
	cache_entry = hash_search(condition_cache, &key, HASH_ENTER, &found);
	Assert(!found);
	cache_entry->id = id;
	return id;
}

static PlanReplayCardinalityId
relation_token(PlannerInfo *root, Index rti)
{
	RelationCacheKey key;
	RelationCacheEntry *cache_entry;
	RangeTblEntry *rte;
	StringInfoData descriptor;
	Node	   *semantics = NULL;
	char	   *node_text = NULL;
	PlanReplayCardinalityId id;
	bool		found;

	MemSet(&key, 0, sizeof(key));
	key.root = root;
	key.rti = rti;
	cache_entry = hash_search(relation_cache, &key, HASH_FIND, NULL);
	if (cache_entry != NULL)
		return cache_entry->id;
	if (rti <= 0 || rti >= root->simple_rel_array_size)
		elog(ERROR, "cannot identify RT index %u for cardinality ID", rti);
	rte = planner_rt_fetch(rti, root);
	/*
	 * RTI 必须和 query_level 一起使用：不同查询层可以重复使用相同 RTI。
	 * 普通表记录 OID；SUBQUERY、FUNCTION、VALUES 等没有稳定关系 OID 的 RTE
	 * 则追加自身的规范语义，避免只按 RTE 类型发生误命中。
	 */
	initStringInfo(&descriptor);
	appendStringInfo(&descriptor,
					 "relation|level=%d|rti=%u|rtekind=%d|relid=%u|inh=%d",
					 root->query_level, rti, (int) rte->rtekind,
					 rte->rtekind == RTE_RELATION ? rte->relid : InvalidOid,
					 rte->inh ? 1 : 0);
	switch (rte->rtekind)
	{
		case RTE_SUBQUERY:
			semantics = (Node *) rte->subquery;
			break;
		case RTE_FUNCTION:
			semantics = (Node *) rte->functions;
			appendStringInfo(&descriptor, "|ordinality=%d", rte->funcordinality);
			break;
		case RTE_TABLEFUNC:
			semantics = (Node *) rte->tablefunc;
			break;
		case RTE_VALUES:
			semantics = (Node *) rte->values_lists;
			break;
		case RTE_CTE:
			appendStringInfo(&descriptor, "|cte=%zu:%s|levelsup=%u|self=%d",
						 strlen(rte->ctename), rte->ctename, rte->ctelevelsup,
						 rte->self_reference ? 1 : 0);
			semantics = (Node *) rte->coltypes;
			break;
		case RTE_NAMEDTUPLESTORE:
			appendStringInfo(&descriptor, "|enr=%zu:%s",
						 strlen(rte->enrname), rte->enrname);
			semantics = (Node *) rte->coltypes;
			break;
		default:
			break;
	}
	if (semantics != NULL)
	{
		node_text = node_descriptor(root, semantics);
		appendStringInfo(&descriptor, "|rte_semantics=%zu:%s",
						 strlen(node_text), node_text);
		pfree(node_text);
	}
	id = make_token(descriptor.data);
	pfree(descriptor.data);
	cache_entry = hash_search(relation_cache, &key, HASH_ENTER, &found);
	Assert(!found);
	cache_entry->id = id;
	return id;
}

static PlanReplayCardinalityId *
sort_unique_ids(const PlanReplayCardinalityId *input, int count,
				int *result_count)
{
	PlanReplayCardinalityId *result =
		palloc(sizeof(PlanReplayCardinalityId) * Max(count, 1));
	int			index;
	int			output = 0;

	if (count > 0)
		memcpy(result, input, sizeof(*result) * count);
	qsort(result, count, sizeof(*result), cardinality_id_compare);
	for (index = 0; index < count; index++)
	{
		if (output == 0 || !cardinality_id_equal(result[index], result[output - 1]))
			result[output++] = result[index];
	}
	*result_count = output;
	return result;
}

static PlanReplayCardinalityId *
merge_ids(const PlanReplayCardinalityId *left, int nleft,
		  const PlanReplayCardinalityId *right, int nright,
		  const PlanReplayCardinalityId *extra, int nextra, int *result_count)
{
	int			count = nleft + nright + nextra;
	PlanReplayCardinalityId *result =
		palloc(sizeof(PlanReplayCardinalityId) * Max(count, 1));
	int			positions[3] = {0, 0, 0};
	const PlanReplayCardinalityId *inputs[3] = {left, right, extra};
	int			lengths[3] = {nleft, nright, nextra};
	int			output = 0;

	/* 三个输入都已排序；线性归并同时去重，避免 Join 层反复重排累计状态。 */
	while (positions[0] < lengths[0] || positions[1] < lengths[1] ||
		   positions[2] < lengths[2])
	{
		PlanReplayCardinalityId next;
		bool		have_next = false;
		int			input;
		for (input = 0; input < 3; input++)
		{
			if (positions[input] >= lengths[input])
				continue;
			if (!have_next ||
				cardinality_id_compare(&inputs[input][positions[input]],
								   &next) < 0)
			{
				next = inputs[input][positions[input]];
				have_next = true;
			}
		}
		Assert(have_next);
		if (output == 0 || !cardinality_id_equal(next, result[output - 1]))
			result[output++] = next;
		for (input = 0; input < 3; input++)
		{
			while (positions[input] < lengths[input] &&
				   cardinality_id_equal(inputs[input][positions[input]], next))
				positions[input]++;
		}
	}
	*result_count = output;
	return result;
}

static PlanReplayCardinalityId
hash_state(CardinalityEstimateKind kind,
		   PlanReplayCardinalityId *relations, int nrelations,
		   PlanReplayCardinalityId *conditions, int nconditions)
{
	/*
	 * 这里只输入类型和规范 token 集合。native/effective rows、cost、width、
	 * sequence、Path 类型等物理或调用时信息都不得影响 cardinality_id。
	 */
	pg_cryptohash_ctx *hash = begin_hash("pg_plan_replay_cardinality_id_v1");
	int			index;

	hash_uint32(hash, (uint32) kind);
	hash_uint32(hash, (uint32) nrelations);
	for (index = 0; index < nrelations; index++)
	{
		hash_uint64(hash, relations[index].hi);
		hash_uint64(hash, relations[index].lo);
	}
	hash_uint32(hash, (uint32) nconditions);
	for (index = 0; index < nconditions; index++)
	{
		hash_uint64(hash, conditions[index].hi);
		hash_uint64(hash, conditions[index].lo);
	}
	return finish_hash(hash);
}

static char *
state_descriptor(CardinalityEstimateKind kind,
			 PlanReplayCardinalityId *relations, int nrelations,
			 PlanReplayCardinalityId *conditions, int nconditions)
{
	StringInfoData descriptor;
	char text[33];
	int			index;

	initStringInfo(&descriptor);
	appendStringInfo(&descriptor, "cardinality|version=1|kind=%d|relations=[",
					 (int) kind);
	for (index = 0; index < nrelations; index++)
	{
		plan_replay_format_cardinality_id(relations[index], text);
		appendStringInfo(&descriptor, "%s;", text);
	}
	appendStringInfoString(&descriptor, "]|conditions=[");
	for (index = 0; index < nconditions; index++)
	{
		plan_replay_format_cardinality_id(conditions[index], text);
		appendStringInfo(&descriptor, "%s;", text);
	}
	appendStringInfoChar(&descriptor, ']');
	return descriptor.data;
}

static CardinalityStateEntry *
register_state(CardinalityEstimateKind kind,
		   PlanReplayCardinalityId *relations, int nrelations,
		   PlanReplayCardinalityId *conditions, int nconditions)
{
	PlanReplayCardinalityId id = hash_state(kind, relations, nrelations,
										conditions, nconditions);
	CardinalityStateEntry *entry;
	bool		found;

	entry = hash_search(cardinality_state_table, &id, HASH_ENTER, &found);
	if (found)
	{
		/* 相同 ID 必须逐项对应同一规范状态，不能静默接受截断哈希碰撞。 */
		if (entry->kind != kind ||
			entry->nrelations != nrelations ||
			entry->nconditions != nconditions ||
			(nrelations > 0 &&
			 memcmp(entry->relations, relations,
					sizeof(*relations) * nrelations) != 0) ||
			(nconditions > 0 &&
			 memcmp(entry->conditions, conditions,
					sizeof(*conditions) * nconditions) != 0))
		{
			char text[33];

			plan_replay_format_cardinality_id(id, text);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("cardinality ID collision for %s", text),
					 errdetail("Two different canonical cardinality states produced the same 128-bit ID.")));
		}
		pfree(relations);
		pfree(conditions);
		return entry;
	}
	{
		MemoryContext old_context;
		char	   *descriptor = state_descriptor(kind, relations, nrelations,
											  conditions, nconditions);

		old_context = MemoryContextSwitchTo(cardinality_identity_context);
		entry->descriptor = pstrdup(descriptor);
		entry->relations = palloc(sizeof(*relations) * Max(nrelations, 1));
		entry->conditions = palloc(sizeof(*conditions) * Max(nconditions, 1));
		if (nrelations > 0)
			memcpy(entry->relations, relations, sizeof(*relations) * nrelations);
		if (nconditions > 0)
			memcpy(entry->conditions, conditions, sizeof(*conditions) * nconditions);
		MemoryContextSwitchTo(old_context);
		pfree(descriptor);
	}
	entry->kind = kind;
	entry->nrelations = nrelations;
	entry->nconditions = nconditions;
	pfree(relations);
	pfree(conditions);
	return entry;
}

static CardinalityStateEntry *
find_state(PlanReplayCardinalityId id)
{
	CardinalityStateEntry *entry =
		hash_search(cardinality_state_table, &id, HASH_FIND, NULL);

	if (entry == NULL)
	{
		char text[33];

		plan_replay_format_cardinality_id(id, text);
		elog(ERROR, "cardinality state %s is not registered", text);
	}
	return entry;
}

static CardinalityStateEntry *
state_for_rel(PlannerInfo *root, RelOptInfo *rel)
{
	PlanReplayCardinalityId id;

	/* 优先复用估算 hook 已登记的完整累计状态。 */
	if (rel->plan_replay_cardinality_id_valid)
	{
		id.hi = rel->plan_replay_cardinality_id_hi;
		id.lo = rel->plan_replay_cardinality_id_lo;
		return find_state(id);
	}
	if (IS_SIMPLE_REL(rel) && rel->relid > 0)
	{
		/*
		 * 该兜底路径用于尚未携带 ID 的简单基表输入；它仍然把基表过滤条件
		 * 纳入状态，绝不能退化成仅按 relids 识别。
		 */
		PlanReplayCardinalityId relation = relation_token(root, rel->relid);
		PlanReplayCardinalityId *relations;
		PlanReplayCardinalityId *conditions;
		PlanReplayCardinalityId *extra;
		ListCell   *cell;
		int			nextra = list_length(rel->baserestrictinfo);
		int			nrelations;
		int			nconditions;
		int			index = 0;

		relations = merge_ids(NULL, 0, NULL, 0, &relation, 1, &nrelations);
		extra = palloc(sizeof(*extra) * Max(nextra, 1));
		foreach(cell, rel->baserestrictinfo)
			extra[index++] = condition_token(root,
											 lfirst_node(RestrictInfo, cell),
											 CARDINALITY_CONDITION_FILTER);
		conditions = sort_unique_ids(extra, nextra, &nconditions);
		pfree(extra);
		return register_state(CARDINALITY_ESTIMATE_BASE, relations, nrelations,
							  conditions, nconditions);
	}
	elog(ERROR, "input relation has no cardinality ID");
	return NULL;
}

static CardinalityStateEntry *
state_for_path(PlannerInfo *root, Path *path)
{
	PlanReplayCardinalityId id;

	/* 参数化 Path 的语义状态属于 ParamPathInfo，而非普通父 RelOptInfo。 */
	if (path->plan_replay_cardinality_id_valid)
	{
		id.hi = path->plan_replay_cardinality_id_hi;
		id.lo = path->plan_replay_cardinality_id_lo;
		return find_state(id);
	}
	if (path->param_info != NULL &&
		path->param_info->plan_replay_cardinality_id_valid)
	{
		id.hi = path->param_info->plan_replay_cardinality_id_hi;
		id.lo = path->param_info->plan_replay_cardinality_id_lo;
		return find_state(id);
	}
	return state_for_rel(root, path->parent);
}

static PlanReplayCardinalityId
required_outer_token(PlannerInfo *root, Relids required_outer)
{
	StringInfoData descriptor;
	int			rti = -1;
	char text[33];

	/* 使用外部关系的 relation token，而不是只记录易跨查询层歧义的裸 RTI。 */
	initStringInfo(&descriptor);
	appendStringInfoString(&descriptor, "required_outer=[");
	while ((rti = bms_next_member(required_outer, rti)) >= 0)
	{
		PlanReplayCardinalityId relation = relation_token(root, rti);

		plan_replay_format_cardinality_id(relation, text);
		appendStringInfo(&descriptor, "%s;", text);
	}
	appendStringInfoChar(&descriptor, ']');
	{
		PlanReplayCardinalityId id = make_token(descriptor.data);

		pfree(descriptor.data);
		return id;
	}
}

static PlanReplayCardinalityId
join_boundary_token(PlannerInfo *root, CardinalityEstimateContext *context,
					CardinalityStateEntry *outer,
					CardinalityStateEntry *inner,
					PlanReplayCardinalityId *layer_conditions,
					int nlayer_conditions)
{
	StringInfoData descriptor;
	char outer_text[33];
	char inner_text[33];
	char condition_text[33];
	SpecialJoinInfo *sjinfo = context->sjinfo;
	int			index;
	PlanReplayCardinalityId id;

	/*
	 * INNER JOIN 的关系和条件集合可交换、可结合，不需要边界 token。这里专门
	 * 固化非 INNER JOIN 的左右方向、OJ 标识及 SpecialJoinInfo 约束，并把本层
	 * ON/filter 角色化后的条件包含进来。
	 */
	plan_replay_format_cardinality_id(outer->id, outer_text);
	plan_replay_format_cardinality_id(inner->id, inner_text);
	initStringInfo(&descriptor);
	appendStringInfo(&descriptor,
					 "join_boundary|type=%d|outer=%s|inner=%s|ojrelid=%u"
					 "|lhs_strict=%d|min_left=",
					 (int) sjinfo->jointype, outer_text, inner_text, sjinfo->ojrelid,
					 sjinfo->lhs_strict ? 1 : 0);
	{
		char	   *value = nodeToString(sjinfo->min_lefthand);

		appendStringInfo(&descriptor, "%zu:%s|min_right=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->min_righthand);
		appendStringInfo(&descriptor, "%zu:%s|syn_left=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->syn_lefthand);
		appendStringInfo(&descriptor, "%zu:%s|syn_right=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->syn_righthand);
		appendStringInfo(&descriptor, "%zu:%s|commute_above_l=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->commute_above_l);
		appendStringInfo(&descriptor, "%zu:%s|commute_above_r=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->commute_above_r);
		appendStringInfo(&descriptor, "%zu:%s|commute_below_l=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->commute_below_l);
		appendStringInfo(&descriptor, "%zu:%s|commute_below_r=", strlen(value), value);
		pfree(value);
		value = nodeToString(sjinfo->commute_below_r);
		appendStringInfo(&descriptor,
						 "%zu:%s|semi_btree=%d|semi_hash=%d|semi_operators=",
						 strlen(value), value, sjinfo->semi_can_btree ? 1 : 0,
						 sjinfo->semi_can_hash ? 1 : 0);
		pfree(value);
		value = nodeToString(sjinfo->semi_operators);
		appendStringInfo(&descriptor, "%zu:%s|semi_rhs=", strlen(value), value);
		pfree(value);
		value = node_descriptor(root, (Node *) sjinfo->semi_rhs_exprs);
		appendStringInfo(&descriptor, "%zu:%s|conditions=[", strlen(value), value);
		pfree(value);
	}
	for (index = 0; index < nlayer_conditions; index++)
	{
		plan_replay_format_cardinality_id(layer_conditions[index], condition_text);
		appendStringInfo(&descriptor, "%s;", condition_text);
	}
	appendStringInfoChar(&descriptor, ']');
	id = make_token(descriptor.data);
	pfree(descriptor.data);
	return id;
}

PlanReplayCardinalityId
plan_replay_compute_cardinality_id(PlannerInfo *root,
								   CardinalityEstimateContext *context)
{
	CardinalityStateEntry *outer = NULL;
	CardinalityStateEntry *inner = NULL;
	PlanReplayCardinalityId *relations;
	PlanReplayCardinalityId *conditions;
	PlanReplayCardinalityId *layer;
	PlanReplayCardinalityId extra[2];
	ListCell   *cell;
	int			nrelations;
	int			nconditions;
	int			nlayer = list_length(context->restrictlist);
	int			nextra = 0;
	int			index = 0;
	CardinalityStateEntry *state;

	/*
	 * 第一步：把当前层 restrictlist 转成带角色的条件 token。子关系以前层次
	 * 已生效的条件随后从各自 state 合并进来，因此最终身份覆盖从叶子到当前
	 * 关系的完整条件集合，而不只是本层 restrictlist。
	 */
	layer = palloc(sizeof(*layer) * Max(nlayer, 1));
	foreach(cell, context->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, cell);
		CardinalityConditionRole role = CARDINALITY_CONDITION_QUAL;

		if (context->kind == CARDINALITY_ESTIMATE_BASE)
			role = CARDINALITY_CONDITION_FILTER;
		else if (context->kind == CARDINALITY_ESTIMATE_PARAMETERIZED_BASE)
			role = bms_is_subset(rinfo->required_relids, context->rel->relids) ?
				CARDINALITY_CONDITION_FILTER : CARDINALITY_CONDITION_PARAMETER;
		else if (context->kind == CARDINALITY_ESTIMATE_PARAMETERIZED_JOIN &&
				 !bms_is_subset(rinfo->required_relids, context->rel->relids))
			role = CARDINALITY_CONDITION_PARAMETER;
		else if (context->sjinfo->jointype != JOIN_INNER)
			role = RINFO_IS_PUSHED_DOWN(rinfo, context->rel->relids) ?
				CARDINALITY_CONDITION_FILTER : CARDINALITY_CONDITION_JOINQUAL;
		layer[index++] = condition_token(root, rinfo, role);
	}
	{
		PlanReplayCardinalityId *normalized =
			sort_unique_ids(layer, nlayer, &index);

		pfree(layer);
		layer = normalized;
		nlayer = index;
		index = 0;
	}

	/* 第二步：基表创建初始状态；Join 对两个输入的累计状态做集合并集。 */
	if (context->kind == CARDINALITY_ESTIMATE_BASE ||
		context->kind == CARDINALITY_ESTIMATE_PARAMETERIZED_BASE)
	{
		PlanReplayCardinalityId relation = relation_token(root, context->rel->relid);

		relations = merge_ids(NULL, 0, NULL, 0, &relation, 1, &nrelations);
		conditions = merge_ids(NULL, 0, NULL, 0, layer, nlayer, &nconditions);
	}
	else
	{
		outer = context->outer_path != NULL ?
			state_for_path(root, context->outer_path) :
			state_for_rel(root, context->outer_rel);
		inner = context->inner_path != NULL ?
			state_for_path(root, context->inner_path) :
			state_for_rel(root, context->inner_rel);
		relations = merge_ids(outer->relations, outer->nrelations,
						  inner->relations, inner->nrelations,
						  NULL, 0, &nrelations);
		conditions = merge_ids(outer->conditions, outer->nconditions,
						   inner->conditions, inner->nconditions,
						   layer, nlayer, &nconditions);
		if (context->sjinfo->jointype != JOIN_INNER)
		{
			/* 非 INNER JOIN 还必须保留有向边界，不能只看无序关系集合。 */
			extra[nextra++] = join_boundary_token(root, context, outer, inner,
											 layer, nlayer);
			{
				PlanReplayCardinalityId *merged =
					merge_ids(conditions, nconditions, NULL, 0,
							  extra, nextra, &index);

				pfree(conditions);
				conditions = merged;
				nconditions = index;
			}
		}
	}

	/*
	 * 第三步：参数化状态额外加入 required_outer。实际执行的参数条件已在
	 * restrictlist 中以 PARAMETER 角色进入 layer，两者缺一都会导致误匹配。
	 */
	if (context->kind == CARDINALITY_ESTIMATE_PARAMETERIZED_BASE ||
		context->kind == CARDINALITY_ESTIMATE_PARAMETERIZED_JOIN)
	{
		PlanReplayCardinalityId required =
			required_outer_token(root, context->required_outer);
		PlanReplayCardinalityId *merged =
			merge_ids(conditions, nconditions, NULL, 0, &required, 1, &index);

		pfree(conditions);
		conditions = merged;
		nconditions = index;
	}
	pfree(layer);
	state = register_state(context->kind, relations, nrelations,
						   conditions, nconditions);
	return state->id;
}

PlanReplayCardinalityId
plan_replay_cardinality_qual_fingerprint(PlanReplayCardinalityId cardinality_id)
{
	CardinalityStateEntry *state = find_state(cardinality_id);
	pg_cryptohash_ctx *hash = begin_hash("pg_plan_replay_logical_quals_v1");
	int			index;

	/* 该指纹仅供 JSONL 校对完整条件集合，不作为 replay 查找键。 */
	hash_uint32(hash, state->nconditions);
	for (index = 0; index < state->nconditions; index++)
	{
		hash_uint64(hash, state->conditions[index].hi);
		hash_uint64(hash, state->conditions[index].lo);
	}
	return finish_hash(hash);
}

void
plan_replay_cardinality_identity_begin(bool replay_mode)
{
	HASHCTL		control;
	MemoryContext old_context;

	/* 所有表都属于一次规划语句；结束时整体释放，地址/serial 不跨语句复用。 */
	cardinality_replay_mode = replay_mode;
	Assert(cardinality_identity_context == NULL);
	cardinality_identity_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "pg_plan_replay cardinality identity",
							  ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(cardinality_identity_context);
	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(PlanReplayCardinalityId);
	control.entrysize = sizeof(CardinalityTokenEntry);
	cardinality_token_table = hash_create("pg_plan_replay cardinality tokens",
										 1024, &control, HASH_ELEM | HASH_BLOBS);
	control.entrysize = sizeof(CardinalityStateEntry);
	cardinality_state_table = hash_create("pg_plan_replay cardinality states",
										 1024, &control, HASH_ELEM | HASH_BLOBS);
	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(ClauseCacheKey);
	control.entrysize = sizeof(ClauseCacheEntry);
	clause_cache = hash_create("pg_plan_replay cardinality clauses", 1024,
							 &control, HASH_ELEM | HASH_BLOBS);
	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(ConditionCacheKey);
	control.entrysize = sizeof(ConditionCacheEntry);
	condition_cache = hash_create("pg_plan_replay cardinality conditions", 1024,
								&control, HASH_ELEM | HASH_BLOBS);
	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(ObjectCacheKey);
	control.entrysize = sizeof(ObjectCacheEntry);
	eclass_cache = hash_create("pg_plan_replay cardinality ECs", 128,
						 &control, HASH_ELEM | HASH_BLOBS);
	MemSet(&control, 0, sizeof(control));
	control.keysize = sizeof(RelationCacheKey);
	control.entrysize = sizeof(RelationCacheEntry);
	relation_cache = hash_create("pg_plan_replay cardinality relations", 128,
						   &control, HASH_ELEM | HASH_BLOBS);
	MemoryContextSwitchTo(old_context);
}

void
plan_replay_cardinality_identity_end(void)
{
	if (cardinality_identity_context != NULL)
		MemoryContextDelete(cardinality_identity_context);
	cardinality_identity_context = NULL;
	cardinality_token_table = NULL;
	cardinality_state_table = NULL;
	clause_cache = NULL;
	condition_cache = NULL;
	eclass_cache = NULL;
	relation_cache = NULL;
	cardinality_replay_mode = false;
}
