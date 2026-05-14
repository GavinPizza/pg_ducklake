# Root Makefile. Three roles:
#
# 1. Delegating: `make examples/<name>/<target>` forwards to
#    examples/<name>/Makefile via $(MAKE) -C.
# 2. libpgddb source list: extension Makefiles `include` this file to pull
#    in PGDDB_INCLUDE / PGDDB_OBJS / PGDDB_DUCKDB_INCLUDE. They append
#    PGDDB_OBJS to OBJS so the libpgddb sources get bundled into their dylib.
# 3. DuckDB submodule + build. The submodule lives at $(PGDDB_DIR)/third_party/duckdb
#    and is shared across consumers. Consumers pass their own
#    EXTENSION_CONFIGS=/abs/path/to/their_extensions.cmake before invoking
#    `duckdb` / `install-duckdb`. NOTE: cmake caches extension choices in the
#    build dir, so switching EXTENSION_CONFIGS between consumers on the same
#    machine requires `make clean-duckdb` first.

PGDDB_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PGDDB_INCLUDE := -I$(PGDDB_DIR)/include
PGDDB_DUCKDB_INCLUDE := -isystem $(PGDDB_DIR)/third_party/duckdb/src/include \
                        -isystem $(PGDDB_DIR)/third_party/duckdb/third_party/re2
PGDDB_CPP_SRCS := $(wildcard $(PGDDB_DIR)/src/*.cpp $(PGDDB_DIR)/src/*/*.cpp)
PGDDB_C_SRCS := $(wildcard $(PGDDB_DIR)/src/*.c $(PGDDB_DIR)/src/*/*.c)
PGDDB_SRCS := $(PGDDB_CPP_SRCS) $(PGDDB_C_SRCS)
PGDDB_OBJS := $(PGDDB_CPP_SRCS:.cpp=.o) $(PGDDB_C_SRCS:.c=.o)

# --- DuckDB submodule build ---

# set to `make` to disable ninja
DUCKDB_GEN ?= ninja
# used to know what version of extensions to download
DUCKDB_VERSION = v1.4.3
# duckdb build tweaks
DUCKDB_CMAKE_VARS = -DCXX_EXTRA=-fvisibility=default -DBUILD_SHELL=0 -DBUILD_PYTHON=0 -DBUILD_UNITTESTS=0
# set to 1 to disable asserts in DuckDB. This is particularly useful in combinition with MotherDuck.
# When asserts are enabled the released motherduck extension will fail some of
# those asserts. By disabling asserts it's possible to run a debug build of
# DuckDB agains the release build of MotherDuck.
DUCKDB_DISABLE_ASSERTIONS ?= 0

DUCKDB_BUILD_CXX_FLAGS=
DUCKDB_BUILD_TYPE=
ifeq ($(DUCKDB_BUILD), Debug)
	DUCKDB_BUILD_CXX_FLAGS = -g -O0 -D_GLIBCXX_ASSERTIONS
	DUCKDB_BUILD_TYPE = debug
	DUCKDB_MAKE_TARGET = debug
else ifeq ($(DUCKDB_BUILD), ReleaseStatic)
	DUCKDB_BUILD_CXX_FLAGS =
	DUCKDB_BUILD_TYPE = release
	DUCKDB_MAKE_TARGET = bundle-library
else
	DUCKDB_BUILD_CXX_FLAGS =
	DUCKDB_BUILD_TYPE = release
	DUCKDB_MAKE_TARGET = release
endif

DUCKDB_BUILD_DIR = $(PGDDB_DIR)/third_party/duckdb/build/$(DUCKDB_BUILD_TYPE)

ifeq ($(DUCKDB_BUILD), ReleaseStatic)
	FULL_DUCKDB_LIB = $(DUCKDB_BUILD_DIR)/libduckdb_bundle.a
else
	FULL_DUCKDB_LIB = $(DUCKDB_BUILD_DIR)/src/libduckdb$(DLSUFFIX)
endif

# Consumer-provided absolute path to its *_extensions.cmake. Empty = no
# third-party extensions baked in.
EXTENSION_CONFIGS ?=

.PHONY: duckdb install-duckdb clean-duckdb

duckdb: $(FULL_DUCKDB_LIB)

$(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD:
	git -C $(PGDDB_DIR) submodule update --init --recursive

$(FULL_DUCKDB_LIB): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD $(EXTENSION_CONFIGS)
ifeq ($(DUCKDB_BUILD), ReleaseStatic)
	mkdir -p $(PGDDB_DIR)/third_party/duckdb/build/release/vcpkg_installed
endif
	OVERRIDE_GIT_DESCRIBE=$(DUCKDB_VERSION) \
	GEN=$(DUCKDB_GEN) \
	CMAKE_VARS="$(DUCKDB_CMAKE_VARS)" \
	DISABLE_SANITIZER=1 \
	DISABLE_ASSERTIONS=$(DUCKDB_DISABLE_ASSERTIONS) \
	EXTENSION_CONFIGS="$(EXTENSION_CONFIGS)" \
	$(MAKE) -C $(PGDDB_DIR)/third_party/duckdb \
	$(DUCKDB_MAKE_TARGET)

# install-duckdb is consumer-facing: must be invoked via a consumer Makefile
# that has pgxs loaded so $(install_bin) and $(PG_LIB) are defined. In
# ReleaseStatic mode duckdb is linked into the consumer's .dylib so this is a
# no-op.
install-duckdb: $(FULL_DUCKDB_LIB)
ifneq ($(DUCKDB_BUILD), ReleaseStatic)
	$(install_bin) -m 755 $(FULL_DUCKDB_LIB) $(DESTDIR)$(PG_LIB)
endif

clean-duckdb:
	rm -rf $(PGDDB_DIR)/third_party/duckdb/build

# Delegate make examples/<name>/<target> to examples/<name>/Makefile.
examples/%:
	$(MAKE) -C $(@D) $(@F)
