#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

// Handle COPY <ducklake_table> FROM STDIN; returns rows inserted, creates a snapshot on completion.
uint64_t DucklakeCopyFromStdin(CopyStmt *stmt, const char *query_string);

} // namespace pgducklake
