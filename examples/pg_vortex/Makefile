# Pull in libpgddb sources + duckdb-build wiring from the repo root.
ROOT_DIR := $(abspath $(CURDIR)/../..)
EXTENSION_CONFIGS := $(CURDIR)/pg_vortex_extensions.cmake
include $(ROOT_DIR)/Makefile

MODULE_big = pg_vortex
EXTENSION = pg_vortex
DATA = $(wildcard pg_vortex--*.sql)

SRCS = $(wildcard src/*.cpp src/*/*.cpp)
OBJS = $(SRCS:.cpp=.o)
C_SRCS = $(wildcard src/*.c src/*/*.c)
OBJS += $(C_SRCS:.c=.o)

# libpgddb objects compiled in-place at $(ROOT_DIR)/src/.
OBJS += $(PGDDB_OBJS)

PG_VORTEX_LINK_FLAGS = -lduckdb -Wl,-rpath,$(PG_LIB)/ -L$(DUCKDB_BUILD_DIR)/src -L$(PG_LIB) -lstdc++ -llz4

override PG_CPPFLAGS += -Iinclude $(PGDDB_INCLUDE) $(PGDDB_DUCKDB_INCLUDE) -isystem $(INCLUDEDIR_SERVER)
override PG_CXXFLAGS += -std=c++17 -Wno-register

SHLIB_LINK += $(PG_VORTEX_LINK_FLAGS)

REGRESS = basic
REGRESS_OPTS = --temp-instance=./tmp_check --temp-config=$(CURDIR)/regression.conf

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_LIB := $(shell $(PG_CONFIG) --pkglibdir)
INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
include $(PGXS)

# Consumer .o files depend on the duckdb submodule HEAD marker (defined in
# the root Makefile) so first-time builds init the submodule before
# compiling.
$(OBJS): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD

# shlib pulls in $(FULL_DUCKDB_LIB) before linking.
$(shlib): $(FULL_DUCKDB_LIB) $(OBJS)

# Wire duckdb-lib install into pgxs's install target.
install: install-duckdb

clean-all: clean clean-duckdb
