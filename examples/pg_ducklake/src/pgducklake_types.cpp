/*
 * pgducklake_types.cpp -- libpgddb type and row-deparse hooks.
 *
 * @scope backend: install GetPostgresDuckDBType + ConvertDuckToPostgresValue
 *                 + the row-deparse hooks (var_is_row, func_returns_row,
 *                 write_row_refname, is_fake_type)
 *
 * Two related concerns live here:
 *
 * 1) DuckDB STRUCT/UNION/MAP values need to round-trip through PG as a
 *    real PG type. We map them to ducklake.duckdb_struct (a varlena
 *    passthrough type) and serialize via pgddb::ConvertToStringDatum.
 *
 * 2) DuckLake metadata functions (snapshots, table_info, flush_inlined_data,
 *    duckdb_query, ...) declare PG return type SETOF ducklake.duckdb_row.
 *    The deparser must turn a top-level Var of type duckdb_row into
 *    `<refname>.*` so DuckDB returns all underlying columns, and must
 *    suppress the spurious `::duckdb_row` / `::duckdb_struct` cast that
 *    the standard PG deparser would otherwise emit. Without these hooks
 *    `SELECT * FROM ducklake.duckdb_query('SELECT id,name FROM t')`
 *    returns a single STRUCT column instead of two scalar columns.
 *
 * All hooks chain to a previously-installed prev_hook so multiple libpgddb
 * consumers can coexist in the same backend.
 */

// pgddb_types.hpp pulls in DuckDB headers and must parse before any PG
// header (FATAL macro collision in DuckDB's exception.hpp). Keep it ahead
// of pgddb_ruleutils.h, which includes postgres.h via its own extern "C".
#include "pgddb/pgddb_types.hpp"

#include "pgducklake/pgducklake_types.hpp"
#include "pgducklake/pgducklake_defs.hpp"

extern "C" {
#include "postgres.h"

#include "pgddb/pgddb_ruleutils.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
}

namespace pgducklake {

// Resolve a ducklake.<name> type's OID. Cached in a static after first
// resolution -- pg_type rows don't move during a backend's lifetime.
// Returns InvalidOid before CREATE EXTENSION pg_ducklake.
static Oid LookupDucklakeType(const char *type_name, Oid *cache) {
  if (OidIsValid(*cache))
    return *cache;
  Oid nsp = get_namespace_oid(PGDUCKLAKE_PG_SCHEMA, /*missing_ok=*/true);
  if (!OidIsValid(nsp))
    return InvalidOid;
  Oid type_oid =
      GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, PointerGetDatum(type_name), ObjectIdGetDatum(nsp));
  if (OidIsValid(type_oid))
    *cache = type_oid;
  return type_oid;
}

Oid LookupDucklakeDuckdbRowOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("duckdb_row", &cached);
}

Oid LookupDucklakeDuckdbStructOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("duckdb_struct", &cached);
}

static Oid DucklakeDuckdbRowOid() {
  return LookupDucklakeDuckdbRowOid();
}

static Oid DucklakeDuckdbStructOid() {
  return LookupDucklakeDuckdbStructOid();
}

static Oid DucklakeVariantOid() {
  static Oid cached = InvalidOid;
  return LookupDucklakeType("variant", &cached);
}

// --------------------------------------------------------------------------
// libpgddb type hooks: PG OID -> DuckDB base type, DuckDB type -> PG OID,
// DuckDB value -> PG Datum for ducklake.variant / ducklake.duckdb_struct.
// Each hook handles pg_ducklake's custom types and returns false to decline
// (the kernel then tries the next hook or its built-in fallback).
// --------------------------------------------------------------------------

// ducklake.variant is a varlena text on the PG side. Mapping it to DuckDB's
// VARIANT logical type makes DuckLake store it as variant in its catalog;
// IMPORT FOREIGN SCHEMA queries duckdb_columns() data_type to recover the
// "variant" string (DuckDB's prepared-statement layer returns VARCHAR
// regardless).
static bool ConvertPostgresToBaseDuckColumnTypeHook(Oid pg_oid, duckdb::LogicalType &out) {
  if (OidIsValid(pg_oid) && pg_oid == DucklakeVariantOid()) {
    out = duckdb::LogicalType::VARIANT();
    return true;
  }
  return false;
}

static bool GetPostgresDuckDBTypeHook(const duckdb::LogicalType &type, Oid &out) {
  switch (type.id()) {
  case duckdb::LogicalTypeId::STRUCT:
  case duckdb::LogicalTypeId::UNION:
  case duckdb::LogicalTypeId::MAP: {
    Oid struct_oid = DucklakeDuckdbStructOid();
    if (OidIsValid(struct_oid)) {
      out = struct_oid;
      return true;
    }
    return false;
  }
  case duckdb::LogicalTypeId::VARIANT: {
    Oid variant_oid = DucklakeVariantOid();
    if (OidIsValid(variant_oid)) {
      out = variant_oid;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ConvertDuckToPostgresValueHook(Oid pg_oid, duckdb::Value &value, TupleTableSlot *slot, uint64_t col) {
  if (OidIsValid(pg_oid) &&
      (pg_oid == DucklakeDuckdbStructOid() || pg_oid == DucklakeVariantOid())) {
    slot->tts_values[col] = pgddb::ConvertToStringDatum(value);
    return true;
  }
  return false;
}

// --------------------------------------------------------------------------
// pgddb_ruleutils.h hooks: duckdb_row / variant deparse. Each declines (false /
// unchanged input / NULL) for nodes that aren't pg_ducklake's, so the kernel
// falls through to the next registered hook or its built-in default.
// --------------------------------------------------------------------------

static bool IsFakeTypeHook(Oid type_oid) {
  return OidIsValid(type_oid) && (type_oid == DucklakeDuckdbRowOid() || type_oid == DucklakeDuckdbStructOid() ||
                                  type_oid == DucklakeVariantOid());
}

// get_const_expr otherwise tacks "::ducklake.variant" onto every literal whose
// target column is variant, and DuckDB can't resolve that type. Returning -1
// suppresses the cast. (The generic bare-::numeric suppression now lives in the
// kernel's pgddb_show_type, so it is not repeated here.)
static int ShowTypeHook(Const *constval, int original_showtype) {
  if (constval && IsFakeTypeHook(constval->consttype))
    return -1;
  return original_showtype;
}

static bool VarIsRowHook(Var *var) {
  return var && var->vartype == DucklakeDuckdbRowOid();
}

static bool FuncReturnsRowHook(RangeTblFunction *rtfunc) {
  if (rtfunc && rtfunc->funcexpr && IsA(rtfunc->funcexpr, FuncExpr)) {
    FuncExpr *fexpr = castNode(FuncExpr, rtfunc->funcexpr);
    if (fexpr->funcresulttype == DucklakeDuckdbRowOid())
      return true;
  }
  return false;
}

// Deparse `r['col']` on a duckdb_row Var as `r.col` for DuckDB. r is a
// function alias in the FROM clause whose underlying table function
// expands to real columns, so dot-access is the natural DuckDB syntax.
// Returns the SubscriptingRef with the first index stripped (so any
// trailing nested subscripts still print as `[...]`), or the input sbsref
// unchanged to decline.
static SubscriptingRef *StripFirstSubscriptHook(SubscriptingRef *sbsref, StringInfo buf) {
  if (!sbsref || !IsA(sbsref->refexpr, Var)) {
    return sbsref;
  }
  Var *var = (Var *)sbsref->refexpr;
  if (var->vartype != DucklakeDuckdbRowOid()) {
    return sbsref;
  }
  if (sbsref->refupperindexpr == NIL) {
    return sbsref;
  }
  Const *first = castNode(Const, linitial(sbsref->refupperindexpr));
  Oid typoutput;
  bool typIsVarlena;
  getTypeOutputInfo(first->consttype, &typoutput, &typIsVarlena);
  char *colname = OidOutputFunctionCall(typoutput, first->constvalue);
  appendStringInfo(buf, ".%s", quote_identifier(colname));

  SubscriptingRef *shorter = (SubscriptingRef *)copyObjectImpl(sbsref);
  shorter->refupperindexpr = list_delete_first(shorter->refupperindexpr);
  if (shorter->reflowerindexpr) {
    shorter->reflowerindexpr = list_delete_first(shorter->reflowerindexpr);
  }
  return shorter;
}

// Map ducklake.variant to "VARIANT" in the kernel's CREATE TABLE deparser so
// DuckDB sees the native DuckDB type and stores the column as
// LogicalTypeId::VARIANT instead of silently falling back to VARCHAR.
// This is what lets the round-trip through GetPostgresDuckDBTypeHook above
// see a VARIANT LogicalType for variant columns. It is a DuckdbRuleutils
// virtual override (not a registration hook) because the CREATE TABLE deparser
// is invoked directly by pg_ducklake's DDL path via pgducklake::Ruleutils.
char *Ruleutils::column_type_name(Oid type_oid, int32_t /*typemod*/) {
  if (OidIsValid(type_oid) && type_oid == DucklakeVariantOid()) {
    return pstrdup("VARIANT");
  }
  return NULL;
}

void InitTypeHooks() {
  // Register pg_ducklake's type hooks (DuckDB STRUCT -> ducklake.duckdb_struct,
  // ducklake.variant <-> DuckDB VARIANT) on top of libpgddb's built-in types.
  pgddb::Register_ConvertPostgresToBaseDuckColumnType(ConvertPostgresToBaseDuckColumnTypeHook);
  pgddb::Register_GetPostgresDuckDBType(GetPostgresDuckDBTypeHook);
  pgddb::Register_ConvertDuckToPostgresValue(ConvertDuckToPostgresValueHook);

  // Register pg_ducklake's deparser (ruleutils) hooks. Row-refname `.*` expansion
  // and the bare-::numeric cast suppression are now generic in the kernel.
  Register_pgddb_is_fake_type(IsFakeTypeHook);
  Register_pgddb_show_type(ShowTypeHook);
  Register_pgddb_var_is_duckdb_row(VarIsRowHook);
  Register_pgddb_func_returns_duckdb_row(FuncReturnsRowHook);
  Register_pgddb_strip_first_subscript(StripFirstSubscriptHook);
  // The variant->VARIANT CREATE TABLE mapping is a DuckdbRuleutils virtual
  // override (pgducklake::Ruleutils::column_type_name), not a registration hook.
}

} // namespace pgducklake

#include "pgddb/pgddb_subscript.h"
#include "pgddb/utility/cpp_wrapper.hpp"

extern "C" {

DECLARE_PG_FUNCTION(duckdb_row_in) {
  elog(ERROR, "Creating the ducklake.duckdb_row type is not supported");
}

DECLARE_PG_FUNCTION(duckdb_row_out) {
  elog(ERROR, "Converting a ducklake.duckdb_row to a string is not supported");
}

DECLARE_PG_FUNCTION(duckdb_row_subscript) {
  PG_RETURN_POINTER(&pgddb::pg::duckdb_row_subscript_routines);
}

DECLARE_PG_FUNCTION(duckdb_struct_in) {
  elog(ERROR, "Creating the ducklake.duckdb_struct type is not supported");
}

DECLARE_PG_FUNCTION(duckdb_struct_out) {
  return textout(fcinfo);
}

DECLARE_PG_FUNCTION(duckdb_struct_subscript) {
  PG_RETURN_POINTER(&pgddb::pg::duckdb_struct_subscript_routines);
}
}
