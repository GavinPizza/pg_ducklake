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
// Unlike the deparser hooks in pgddb_ruleutils.h (called deep in vendored
// ruleutils recursion, hence global registration points), these deparsers are
// invoked directly by a consumer, so this is a plain subclassable C++ class with
// no global state.
//
//   class Ruleutils : public pgddb::DuckdbRuleutils {
//   protected:
//     char *column_type_name(Oid type_oid, int32_t typemod) override { ... }
//   };
//   std::string sql = pgducklake::Ruleutils().get_tabledef(relid);
class DuckdbRuleutils {
public:
	virtual ~DuckdbRuleutils() = default;

	// CREATE TABLE emits schema, defaults, NOT NULL / CHECK; no UNIQUE/PK.
	std::string get_tabledef(Oid relation_id);
	std::string get_alter_tabledef(Oid relation_oid, AlterTableStmt *alter_stmt);
	std::string get_rename_relationdef(Oid relation_oid, RenameStmt *rename_stmt);

protected:
	// Map a PG type to its DuckDB CREATE TABLE type name. Return a palloc'd name,
	// or nullptr to fall through to PG's format_type_with_typemod.
	virtual char *
	column_type_name(Oid /*type_oid*/, int32_t /*typemod*/) {
		return nullptr;
	}

	// Validate the relation before CREATE TABLE is generated; ereport(ERROR) to reject.
	virtual void
	validate_create_table(Relation /*relation*/) {
	}
};

} // namespace pgddb
