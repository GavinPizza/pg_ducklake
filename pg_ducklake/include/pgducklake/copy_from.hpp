#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

/*
 * Handle COPY <ducklake_table> FROM STDIN [WITH (options)].
 *
 * Reads tuples via PG's COPY protocol, converts them to inlined table
 * column types, and inserts via table_multi_insert() for native heap
 * performance (batched WAL, BulkInsertState).
 *
 * Creates a DuckLake snapshot on completion.
 *
 * Returns the number of rows inserted (for QueryCompletion).
 */
uint64_t DucklakeCopyFromStdin(CopyStmt *stmt, const char *query_string);

} // namespace pgducklake
