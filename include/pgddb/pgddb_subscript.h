#pragma once

extern "C" {
#include "postgres.h"

#include "nodes/subscripting.h"
}

namespace pgddb {
namespace pg {

/*
 * Hook called by the subscript transform to compute refrestype (the type
 * of `r['col']`). Consumers that expose a polymorphic "unresolved" type
 * (e.g. pg_duckdb's duckdb.unresolved_type) set this to return it.
 * Unset (nullptr) -> lib falls back to the container OID itself.
 */
typedef Oid (*subscript_refrestype_hook_t)(Oid container_oid);
extern subscript_refrestype_hook_t subscript_refrestype_hook;

extern const SubscriptRoutines duckdb_row_subscript_routines;
extern const SubscriptRoutines duckdb_unresolved_type_subscript_routines;
extern const SubscriptRoutines duckdb_struct_subscript_routines;
extern const SubscriptRoutines duckdb_map_subscript_routines;

} // namespace pg
} // namespace pgddb
