#pragma once

#include "pgddb/pgddb_ddl.hpp"
#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

// Lookup OIDs for the pseudo-types declared in pg_ducklake--*.sql.
// Returns InvalidOid before CREATE EXTENSION runs. Cached per-process.
Oid DuckdbRowOid();
Oid DuckdbStructOid();
Oid VariantOid();

class Ruleutils : public pgddb::DuckdbRuleutils {
public:
	// Deparse a CALL of a ducklake-only procedure into the DuckDB statement
	static std::string get_calldef(CallStmt *call);

	// Validate a CREATE INDEX ... USING ducklake_sorted and deparse it to the
	// "ALTER TABLE <rel> SET SORTED BY (...)" statement. Defined in
	// pgducklake_sorted_by.cpp.
	static std::string get_create_sorted_index_def(IndexStmt *stmt);

protected:
	char *column_type_name(Oid type_oid, int32_t typemod) override;
};

} // namespace pgducklake
