MODULE_big = pg_plan_replay
OBJS = pg_plan_replay.o online_prediction.o model_client.o

EXTENSION = pg_plan_replay
DATA = pg_plan_replay--1.0.sql
PGFILEDESC = "pg_plan_replay - planner estimate capture, replay, and online cost prediction"

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
