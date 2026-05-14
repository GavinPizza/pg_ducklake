duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 9c7d34977b10346d0b4cbbde5df807d1dab0b2bf
)
duckdb_extension_load(vortex
    GIT_URL https://github.com/vortex-data/duckdb-vortex
    GIT_TAG d480e1c64840fdef2b051cebeea7a20ddde60962
    SUBMODULES "vortex"
)
