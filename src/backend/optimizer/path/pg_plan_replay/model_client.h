#ifndef PG_PLAN_REPLAY_MODEL_CLIENT_H
#define PG_PLAN_REPLAY_MODEL_CLIENT_H

#include "optimizer/cost.h"

extern bool plan_replay_model_predict(const char *socket_path,
									  int timeout_ms,
									  const char *request,
									  int request_length,
									  int expected_count,
									  Cost *costs,
									  char **error_message);

#endif
