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
