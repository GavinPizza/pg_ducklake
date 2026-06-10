ROOT_DIR := $(abspath $(CURDIR)/../..)
EXTENSION_CONFIGS := $(CURDIR)/pg_ducklake_extensions.cmake
EXTENSION_BUNDLE_EXCLUDE := ducklake postgres_scanner
include $(ROOT_DIR)/Makefile

MODULE_big = pg_ducklake
EXTENSION = pg_ducklake
DATA = pg_ducklake.control $(wildcard sql/pg_ducklake--*.sql)

SRCS = $(wildcard src/*.cpp src/*/*.cpp)
OBJS = $(SRCS:.cpp=.o)
C_SRCS = $(wildcard src/*.c src/*/*.c)
OBJS += $(C_SRCS:.c=.o)

# libpgddb objects compiled in-place at $(ROOT_DIR)/src/.
OBJS += $(PGDDB_OBJS)

# Vendored ducklake: pristine community submodule + ordered customization patches.
# The submodule is pinned to a community duckdb/ducklake commit; our delta lives in
# third_party/ducklake-NNN-<desc>.patch and is applied onto the pristine checkout at build
# time (apply-once, stamp-guarded; see the stamp rule below and CLAUDE.md).
DUCKLAKE_DIR := $(CURDIR)/third_party/ducklake
DUCKLAKE_PATCHES := $(sort $(wildcard $(CURDIR)/third_party/ducklake-*.patch))
DUCKLAKE_STAMP := $(DUCKLAKE_DIR)/.patched
DUCKLAKE_SUBMODULE_HEAD := $(ROOT_DIR)/.git/modules/examples/pg_ducklake/third_party/ducklake/HEAD
DUCKLAKE_INCLUDE = -I$(DUCKLAKE_DIR)/src/include

override PG_CPPFLAGS += -Iinclude $(PGDDB_INCLUDE) $(PGDDB_DUCKDB_INCLUDE) $(DUCKLAKE_INCLUDE) -isystem $(INCLUDEDIR_SERVER)
override PG_CXXFLAGS += -std=c++17 -Wno-register

DUCKLAKE_EXT_LIB := $(DUCKDB_BUILD_DIR)/extension/ducklake/libducklake_extension.a

POSTGRES_SCANNER_EXT_LIB := $(DUCKDB_BUILD_DIR)/extension/postgres_scanner/libpostgres_scanner_extension.a

# DuckLake >= v1.5 needs CRoaring for deletion vectors: the vendored ducklake's
# CMakeLists does find_package(roaring CONFIG REQUIRED), declared in its
# vcpkg.json. Mirroring upstream ducklake CI (its .github/workflows/Catalogs.yml),
# a vcpkg toolchain provided via VCPKG_TOOLCHAIN_PATH (set job-wide in our CI) is
# passed into the duckdb configure with the vendored ducklake dir as the manifest
# dir, so vcpkg builds roaring during configure. These are the same variables
# duckdb's own Makefile wires for VCPKG builds ("Enable VCPKG for this build" in
# third_party/duckdb/Makefile). Appending after the root include is safe: the
# variable is only expanded in the duckdb configure recipe. Without a toolchain
# (local macOS dev) find_package falls back to a system croaring (homebrew).
ifneq ($(VCPKG_TOOLCHAIN_PATH),)
DUCKDB_EXTRA_CMAKE_VARS += -DCMAKE_TOOLCHAIN_FILE='$(VCPKG_TOOLCHAIN_PATH)' -DVCPKG_BUILD=1
DUCKDB_EXTRA_CMAKE_VARS += -DVCPKG_MANIFEST_DIR='$(DUCKLAKE_DIR)'
ifneq ($(VCPKG_TARGET_TRIPLET),)
DUCKDB_EXTRA_CMAKE_VARS += -DVCPKG_TARGET_TRIPLET='$(VCPKG_TARGET_TRIPLET)'
endif
# vcpkg manifest mode installs static deps under the duckdb build dir.
ROARING_LIB_DIR := $(DUCKDB_BUILD_DIR)/vcpkg_installed/$(VCPKG_TARGET_TRIPLET)/lib
endif

PG_DUCKLAKE_LINK_FLAGS = $(FULL_DUCKDB_LIB)
ifeq ($(shell uname -s),Darwin)
	PG_DUCKLAKE_LINK_FLAGS += -Wl,-force_load,$(DUCKLAKE_EXT_LIB)
	PG_DUCKLAKE_LINK_FLAGS += -Wl,-force_load,$(POSTGRES_SCANNER_EXT_LIB)
else
	PG_DUCKLAKE_LINK_FLAGS += -Wl,--whole-archive $(DUCKLAKE_EXT_LIB) $(POSTGRES_SCANNER_EXT_LIB) -Wl,--no-whole-archive
endif
# -lroaring: vcpkg-built in CI (see above), homebrew croaring on local macOS
# (resolved via LIBRARY_PATH=/opt/homebrew/lib). It must come AFTER the
# whole-archive'd extension archives: GNU ld resolves static archives in a
# single left-to-right pass, so a libroaring.a listed earlier contributes
# nothing (no roaring symbol is undefined yet at that point) and the ducklake
# extension's references would stay unresolved.
PG_DUCKLAKE_LINK_FLAGS += -lssl -lcrypto -lstdc++ -llz4
ifneq ($(ROARING_LIB_DIR),)
	PG_DUCKLAKE_LINK_FLAGS += -L$(ROARING_LIB_DIR)
endif
PG_DUCKLAKE_LINK_FLAGS += -lroaring
ifneq ($(shell uname -s),Darwin)
	PG_DUCKLAKE_LINK_FLAGS += -Wl,--exclude-libs,ALL
endif

SHLIB_LINK += $(PG_DUCKLAKE_LINK_FLAGS)

# `make clean` drops the patch stamp so the next build re-applies onto a fresh submodule reset.
EXTRA_CLEAN += $(DUCKLAKE_STAMP)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
include $(PGXS)

# Init the ducklake submodule non-recursively: its inner duckdb / extension-ci-tools
# submodules are only for ducklake's own CI; the SOURCE_DIR extension build uses src/ only.
$(DUCKLAKE_SUBMODULE_HEAD):
	git -C $(ROOT_DIR) submodule update --init examples/pg_ducklake/third_party/ducklake

# Apply our customization patches onto the pristine ducklake submodule (apply-once,
# stamp-guarded). Reset to the pinned commit first so editing a patch and rebuilding
# re-applies from a clean base. The submodule working tree is intentionally left dirty
# (patched); the gitlink stays at the pinned community commit.
$(DUCKLAKE_STAMP): $(DUCKLAKE_PATCHES) $(DUCKLAKE_SUBMODULE_HEAD)
	git -C $(DUCKLAKE_DIR) checkout -q .
	git -C $(DUCKLAKE_DIR) clean -fdq src
	@for p in $(DUCKLAKE_PATCHES); do \
		echo "  ducklake patch: $$(basename $$p)"; \
		git -C $(DUCKLAKE_DIR) apply -p1 --whitespace=nowarn $$p || exit 1; \
	done
	touch $@

# Consumer .o files include patched ducklake headers and the duckdb build configures
# ducklake via SOURCE_DIR, so both must see the patched source: gate them on the stamp
# (which transitively inits the submodule) and on the duckdb submodule HEAD marker.
$(OBJS): $(PGDDB_DIR)/.git/modules/third_party/duckdb/HEAD $(DUCKLAKE_STAMP)
$(FULL_DUCKDB_LIB): $(DUCKLAKE_STAMP)

# The ducklake / postgres_scanner extension archives are produced as a side effect of the duckdb
# cmake build ($(FULL_DUCKDB_LIB)) and have no rule of their own. Declare that so a clean build
# (empty build dir, e.g. CI) builds duckdb first instead of aborting with "No rule to make target
# .../libducklake_extension.a" while resolving the $(shlib) prerequisites below. (Locally it was
# masked by a pre-populated build dir.)
$(DUCKLAKE_EXT_LIB) $(POSTGRES_SCANNER_EXT_LIB): $(FULL_DUCKDB_LIB)

# shlib pulls in $(FULL_DUCKDB_LIB) before linking. The ducklake / postgres_scanner
# extension archives are force_load'd via SHLIB_LINK, so list them as prerequisites
# too -- otherwise rebuilding only the vendored ducklake source (which regenerates
# these .a's but no consumer .o) would not relink the dylib.
$(shlib): $(FULL_DUCKDB_LIB) $(DUCKLAKE_EXT_LIB) $(POSTGRES_SCANNER_EXT_LIB) $(OBJS)

# Project formatting (project files only, not third_party/).
FORMAT_FILES = $(shell find src/ include/ -name '*.cpp' -o -name '*.hpp' -o -name '*.h')

.PHONY: format check-format check-regression clean-regression check-isolation clean-isolation clean-all

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run -Werror $(FORMAT_FILES)

installcheck: all install
	$(MAKE) check-regression check-isolation

check-regression:
	$(MAKE) -C test/regression check-regression

clean-regression:
	$(MAKE) -C test/regression clean-regression

check-isolation:
	$(MAKE) -C test/isolation check-isolation

clean-isolation:
	$(MAKE) -C test/isolation clean-isolation

clean-all: clean clean-regression clean-isolation clean-duckdb
