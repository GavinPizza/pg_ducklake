# DUCKDB_EXTENSION_CONFIGS file consumed by the root Makefile's duckdb build.
# Builds the ducklake DuckDB extension as a standalone libducklake_extension.a
# (DONT_LINK keeps it out of libduckdb_bundle.a). pg_ducklake's Makefile then
# force-loads libducklake_extension.a, ensuring pg_ducklake.cpp's vtable
# references to DuckLakeMetadataManager pull in the matching .o's exactly
# once -- if we also bundled ducklake we'd get multiple-definition errors
# on Linux's strict linker.

duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(postgres_scanner
    DONT_LINK
    GIT_URL https://github.com/duckdb/duckdb-postgres
    GIT_TAG c89234f0b1985f4ee0f52f16e742a1ab2d4ae4f0
    SUBMODULES database-connector
)
duckdb_extension_load(ducklake
    DONT_LINK
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/ducklake
)
