# DuckDB extensions to bake into pg_vortex's bundled duckdb.
#
# NOTE: pg_duckdb and pg_vortex share the same third_party/duckdb/build dir
# (a known limitation; Phase D should give each consumer its own build dir).
# We bundle pg_duckdb's extension set so building pg_vortex doesn't break
# pg_duckdb.
#
# The duckdb-vortex extension (https://github.com/vortex-data/duckdb-vortex)
# uses a Cargo workspace layout that currently breaks when fetched via
# duckdb_extension_load (the inner `vortex/Cargo.toml` is in a submodule
# that the duckdb extension-loader doesn't init). For the MVP demo we skip
# baking it in; the smoke test verifies the planner-hook offload path by
# catching DuckDB's "function not found" error -- which is itself proof
# that the query reached DuckDB instead of the C-side marker stub.
duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 9c7d34977b10346d0b4cbbde5df807d1dab0b2bf
)
