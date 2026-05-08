# Root Makefile. Two roles:
#
# 1. Delegating: `make examples/<name>/<target>` forwards to
#    examples/<name>/Makefile via $(MAKE) -C.
# 2. libpgddb source list: extension Makefiles `include` this file to pull
#    in the variables below. They append PGDDB_OBJS to their OBJS so the
#    libpgddb sources get compiled into their dylib.

PGDDB_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PGDDB_INCLUDE := -I$(PGDDB_DIR)/include
PGDDB_SRCS := $(wildcard $(PGDDB_DIR)/src/*.cpp)
PGDDB_OBJS := $(PGDDB_SRCS:.cpp=.o)

examples/%:
	$(MAKE) -C $(@D) $(@F)
