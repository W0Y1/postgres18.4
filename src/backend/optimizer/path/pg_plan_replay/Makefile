PG_CONFIG ?= pg_config

ifeq ($(PG_PLAN_REPLAY_BUILD),1)

MODULE_big = pg_plan_replay
OBJS = src/pg_plan_replay.o \
	   src/cardinality_identity.o \
	   src/online_prediction.o \
	   src/model_client.o

EXTENSION = pg_plan_replay
DATA = pg_plan_replay--1.0.sql
PGFILEDESC = "pg_plan_replay - planner estimate capture, replay, and online cost prediction"

# Internal headers live beside the C sources instead of the extension root.
PG_CPPFLAGS += -I$(srcdir)/src/include

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

else

# Keep every PGXS product under build/.  The recursive invocation sees this
# Makefile through -f while its current directory is build/, so PGXS uses the
# extension root as srcdir/VPATH and emits objects, bitcode and the shared
# library below build/.
BUILD_DIR := build
FORWARD_TARGETS := all install installcheck check clean distclean \
	maintainer-clean uninstall

.DEFAULT_GOAL := all
.PHONY: $(FORWARD_TARGETS) forward

$(FORWARD_TARGETS): forward

forward:
	@mkdir -p $(BUILD_DIR)/src
	$(MAKE) -C $(BUILD_DIR) -f ../Makefile \
		PG_PLAN_REPLAY_BUILD=1 \
		PG_CONFIG="$(PG_CONFIG)" \
		$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)

endif
