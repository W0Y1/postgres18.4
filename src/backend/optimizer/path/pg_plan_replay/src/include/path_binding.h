#ifndef PG_PLAN_REPLAY_PATH_BINDING_H
#define PG_PLAN_REPLAY_PATH_BINDING_H

#include "nodes/pathnodes.h"

extern void plan_replay_binding_init(void);
extern void plan_replay_binding_fini(void);
extern void plan_replay_binding_begin(void);
extern void plan_replay_binding_end(void);
extern void plan_replay_propagate_path_cardinality(Path *path);

#endif
