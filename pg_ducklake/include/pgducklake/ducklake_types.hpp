#pragma once

#include "pgddb/pgddb_ddl.hpp"
#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

// Cached per-process; returns InvalidOid before CREATE EXTENSION runs.
Oid DucklakeNamespaceOid();
Oid DuckdbRowOid();
Oid UnresolvedTypeOid();
Oid DuckdbStructOid();
Oid VariantOid();

class Ruleutils : public pgddb::DuckdbRuleutils {
public:
	// Deparse a CALL of a ducklake-only procedure into the DuckDB statement.
	static std::string get_calldef(CallStmt *call);

	// Deparse CREATE INDEX ... USING ducklake_sorted to "ALTER TABLE <rel> SET SORTED BY (...)".
	static std::string get_create_sorted_index_def(IndexStmt *stmt);

protected:
	char *column_type_name(Oid type_oid, int32_t typemod) override;
};

} // namespace pgducklake
