CREATE FUNCTION plan_replay_last_timing()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'plan_replay_last_timing'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION plan_replay_reset_stats()
RETURNS void
AS 'MODULE_PATHNAME', 'plan_replay_reset_stats'
LANGUAGE C VOLATILE PARALLEL UNSAFE;
