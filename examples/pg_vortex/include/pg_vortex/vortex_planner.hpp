#pragma once

#include "duckdb.hpp"

#include "pgddb/pg/declarations.hpp"
#include "pgddb/pgddb_duckdb.hpp"

namespace pg_vortex {

PlannedStmt *PlanNode(Query *parse, int cursor_options, bool throw_error);
duckdb::unique_ptr<duckdb::PreparedStatement> Prepare(const Query *query, const char *explain_prefix = nullptr);

} // namespace pg_vortex
