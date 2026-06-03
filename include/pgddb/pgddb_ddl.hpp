#pragma once

#include <cstdint>
#include <string>

#include "pgddb/pg/declarations.hpp" // Oid, Relation

// Postgres parse nodes, used by pointer only.
struct AlterTableStmt;
struct RenameStmt;

namespace pgddb {

// DuckDB DDL deparser: turns a Postgres relation / ALTER / RENAME parsetree into
// the equivalent DuckDB CREATE TABLE / ALTER TABLE / RENAME statement.
//
// Unlike the deparser hooks in pgddb_ruleutils.h (which the vendored ruleutils
// calls deep in its recursion, so they must be global registration points), the
// DDL deparsers are invoked directly by a consumer extension from its own DDL
// path. So this is a plain C++ class: a consumer subclasses it and overrides the
// customization points -- no global state, no registration.
//
//   class Ruleutils : public pgddb::DuckdbRuleutils {
//   protected:
//     char *column_type_name(Oid type_oid, int32_t typemod) override { ... }
//   };
//   std::string sql = pgducklake::Ruleutils().get_tabledef(relid);
class DuckdbRuleutils {
public:
	virtual ~DuckdbRuleutils() = default;

	// CREATE TABLE for the relation (schema, defaults, NOT NULL / CHECK; no
	// UNIQUE/PK). ALTER TABLE and RENAME for the given parsetree.
	std::string get_tabledef(Oid relation_id);
	std::string get_alter_tabledef(Oid relation_oid, AlterTableStmt *alter_stmt);
	std::string get_rename_relationdef(Oid relation_oid, RenameStmt *rename_stmt);

protected:
	// Map a Postgres type to the DuckDB type name written into CREATE TABLE
	// (e.g. ducklake.variant -> "VARIANT"). Return a palloc'd name, or nullptr to
	// fall through to PG's format_type_with_typemod. Base: no override.
	virtual char *column_type_name(Oid /*type_oid*/, int32_t /*typemod*/) {
		return nullptr;
	}

	// Validate the relation just before its CREATE TABLE is generated; ereport(ERROR)
	// to reject (e.g. pg_duckdb's MotherDuck ownership check). Base: accept everything.
	virtual void validate_create_table(Relation /*relation*/) {
	}
};

} // namespace pgddb
