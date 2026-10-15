/* cardinality_identity.c 的扩展内部接口。 */
#ifndef PG_PLAN_REPLAY_CARDINALITY_IDENTITY_H
#define PG_PLAN_REPLAY_CARDINALITY_IDENTITY_H

#include "optimizer/cost.h"

typedef struct PlanReplayCardinalityId
{
	uint64		hi;
	uint64		lo;
} PlanReplayCardinalityId;

/* 每条规划语句各自建立/销毁规范 token、累计状态和碰撞检查表。 */
extern void plan_replay_cardinality_identity_begin(bool replay_mode);
extern void plan_replay_cardinality_identity_end(void);

/* 返回 SHA-256(规范语义状态) 的前 128 位；结果不包含行数或调用顺序。 */
extern PlanReplayCardinalityId plan_replay_compute_cardinality_id(
	PlannerInfo *root, CardinalityEstimateContext *context);

/* 条件指纹供 capture 文件校对，不是稀疏回放键。 */
extern PlanReplayCardinalityId plan_replay_cardinality_qual_fingerprint(
	PlanReplayCardinalityId cardinality_id);
extern void plan_replay_format_cardinality_id(PlanReplayCardinalityId id,
	char output[33]);

#endif
