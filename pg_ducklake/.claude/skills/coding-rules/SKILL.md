---
name: coding-rules
description: "Code style, include order, docs update rules, and submodule policy. MUST consult when: writing or modifying C/C++ files (include order matters), adding/removing/changing ducklake.* SQL functions or procedures (docs/sql_objects.md must be updated), editing third_party/ code, or reviewing code for style."
---

# Coding Rules

## General

- Write clean, minimal code; fewer lines is better
- Prioritize simplicity for effective and maintainable software
- Only include comments that are essential to understanding functionality or convey non-obvious information
- **ASCII only** in all source files, SQL tests, and expected output -- no emojis, no Unicode dashes/quotes (use `-`, `--`, `'`, `"`)

## Lifecycle Scope Decision Guide

When adding new state, ask:

1. **Is it a C++ static variable or static class member?** Register in
   `_PG_init()` (backend process). It survives DuckDB recycle.

2. **Does it depend on the DuckDB instance?** Register on `db.instance`
   in `ducklake_load_extension()` (DuckDB instance). It will be
   re-created on recycle automatically.

3. **Is it SQL catalog state?** Put it in the extension SQL script
   (PG extension). It is created by `CREATE EXTENSION`.

Maintain @scope in the header comment.

## C/C++

- Avoid using `extern "C"` to reference symbols from the same library. Instead, place it at the header file.
- Use `extern "C"` only when necessary, such as when interfacing with third-party libraries.
- Use `namespace pgducklake` for C++ extension code (do not use `namespace pg_ducklake`).
- Use `pgducklake::` when qualifying symbols outside the namespace block.
- Use C++ raw string literals (`R"(...)"`) for multiline SQL; never use adjacent-string concatenation for SQL queries.

## Imports

PostgreSQL and DuckDB headers are conflict-prone. Follow strict include order in mixed files:

1. DuckDB headers
2. DuckLake headers
3. Local `pgducklake` headers
4. PostgreSQL headers last, inside `extern "C"`, must include `<postgres.h>` at first.

**FATAL macro conflict:** PostgreSQL's `elog.h` defines `#define FATAL 22`, which clobbers DuckDB's `ExceptionType::FATAL` enum member in `duckdb/common/exception.hpp`. Any header that transitively includes both will break. The fix is include order: DuckDB's `exception.hpp` (or any header that pulls it in, e.g., `string_util.hpp`, `error_data.hpp`) must be parsed *before* `postgres.h` defines the macro. Once parsed, C++ include guards prevent re-inclusion. Watch for indirect includes -- `pgddb/utility/cpp_wrapper.hpp` and `pgddb/pgddb_node.hpp` include `postgres.h`, so any DuckDB header they transitively need must already be included earlier in the translation unit.

## Third-party Submodules

Two submodules are upstream code -- never commit edits into their trees:

- `duckdb/` (repo root): pinned DuckDB source shared by all extensions.
  Bump the gitlink only.
- `third_party/ducklake`: pristine community checkout. Our divergence
  lives in ordered `third_party/ducklake-NNN-<desc>.patch` files applied
  at build time; change behavior by adding/editing patch files, never by
  committing into the submodule tree.

The pg_duckdb-derived infrastructure is no longer vendored: it is the
libpgddb kernel (`libpgduckdb/` at the repo root, `namespace pgddb`), edited
directly in this repo. When extending it for pg_ducklake:

- Prefer additive hooks, avoid invasive edits.
- Keep diffs minimal and extension-agnostic.
- Ensure zero behavior change when hooks are unused.

## Docs Style

Documentation follows two axes: **AI-oriented** and **human-oriented**.

All docs must be reachable from one of two entrypoints:

### AI docs tree (`CLAUDE.md` entrypoint)

```
CLAUDE.md
  +-- .claude/skills/*          AI workflow guidance
  +-- src/*.cpp header comments  per-file purpose and usage
  +-- test/regression/           self-documenting test cases
  +-- test/isolation/            concurrency test cases
```

To avoid _Docs Rot_, keep AI docs near the code. Do NOT write separate explanation docs or duplicate what code already says. Maintain header comments after each edit. Inline comments only when logic is non-obvious.

### Human docs tree (`README.md` entrypoint)

```
README.md
  +-- docs/README.md              index of all human docs
        +-- docs/sql_objects.md   all SQL objects, functions, and procedures
        +-- docs/settings.md      GUCs and DuckLake options
        +-- docs/access_control.md
        +-- docs/compilation.md
```

Every new doc file must be linked from `docs/README.md`. Keep synced with code:

- When adding, removing, or changing a `ducklake.*` SQL function or procedure in `pg_ducklake--*.sql`, update `docs/sql_objects.md`.
- In reference docs, order TOC tables alphabetically; keep detailed descriptions in logical order.

## Miscellaneous

- When modifying multiple files, run file modification tasks in parallel whenever possible, instead of processing them sequentially
- Prefer additive changes to extension lifecycle.

### PG <-> DuckDB transaction sync

pg_ducklake's writes go through PostgreSQL's heap (inlined data tables) and SPI, so it does **not** use pg_duckdb's command-id / mixed-write tracking -- that machinery was dropped in the libpgddb port (there is no separate DuckDB store to keep in sync).

Instead, PG transaction events are mirrored to DuckDB's DuckLake transaction by `DuckLakeXactCallback` / `DuckLakeSubXactCallback` (@src/duckdb_manager.cpp): PRE_COMMIT/ABORT commit or roll back the DuckDB transaction. SAVEPOINTs taken while DuckDB holds an active transaction are rejected, except around DuckLake's metadata-commit retry loop where they are allowed via `SetAllowSubtransaction(true)`.

Do not change this sync without verifying transaction-safety end-to-end (see `test/isolation/`).
