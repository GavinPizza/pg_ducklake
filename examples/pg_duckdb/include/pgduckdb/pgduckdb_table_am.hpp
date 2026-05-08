#include "pgddb/pg/declarations.hpp"

namespace pgduckdb {
bool IsDuckdbTableAm(const TableAmRoutine *am);

const char *DuckdbTableAmGetName(const TableAmRoutine *am);

const char *DuckdbTableAmGetName(Oid relid);
} // namespace pgduckdb
