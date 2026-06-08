# CLAUDE.md

C/C++ header & include rules (libpgddb core + `examples/*` consumers). PG and
DuckDB headers are include-order-sensitive: PG's `elog.h` `#define FATAL`
clobbers DuckDB's `ExceptionType::FATAL`, so DuckDB headers must precede
`postgres.h`.

## Headers (`.h`/`.hpp`)

- `#pragma once` on the first line; a brief header comment only if needed.
- No `postgres.h` in headers -- get PG structs from
  `include/pgddb/pg/declarations.hpp` (add forward decls there). Exception:
  a PG type that can't be forward-declared (e.g. plain enums).
- Don't declare what `PG_FUNCTION_INFO_V1` already declares.

## Source `.cpp` include groups

Ordered groups, blank line between, alphabetical within each:

1. own/sibling module headers (`"pgducklake/..."`, or `"pgddb/..."` in core)
2. C/C++ std `<...>`
3. postgres-free `"pgddb/..."` (consumers; these pull DuckDB)
4. DuckDB/DuckLake `<...>`
5. `extern "C"` { `postgres.h` first, then PG headers, then postgres-dependent
   pgddb C headers like `pgddb_ruleutils.h` }

Postgres last keeps the FATAL ordering automatic. Postgres-dependent pgddb
*C++* headers (`utility/cpp_wrapper.hpp`, `pgddb_node.hpp`) go between groups
4 and 5.

`.clang-format` uses `SortIncludes: false` / `IncludeBlocks: Preserve` -- order
is manual. ASCII only.
