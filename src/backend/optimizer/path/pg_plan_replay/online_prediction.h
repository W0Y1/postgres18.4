#ifndef PG_PLAN_REPLAY_ONLINE_PREDICTION_H
#define PG_PLAN_REPLAY_ONLINE_PREDICTION_H

#include "nodes/pathnodes.h"

typedef struct PlanReplayOnlineStats
{
	double		inference_ms;
	uint64		batches;
	uint64		paths;
	uint64		applied;
} PlanReplayOnlineStats;

extern void plan_replay_online_init(void);
extern void plan_replay_online_reset_stats(PlanReplayOnlineStats *stats);
extern bool plan_replay_online_predict_paths(PlannerInfo *root,
										RelOptInfo *rel,
										List *paths,
										bool partial,
										PlanReplayOnlineStats *stats);

#endif
