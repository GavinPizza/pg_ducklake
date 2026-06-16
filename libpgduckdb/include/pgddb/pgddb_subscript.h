#pragma once

extern "C" {
#include "postgres.h"

#include "nodes/subscripting.h"
}

namespace pgddb {
namespace pg {

// Computes refrestype (type of `r['col']`); consumers with a polymorphic
// "unresolved" type return it here. nullptr -> fall back to the container OID.
typedef Oid (*subscript_refrestype_hook_t)(Oid container_oid);
extern subscript_refrestype_hook_t subscript_refrestype_hook;

extern const SubscriptRoutines duckdb_row_subscript_routines;
extern const SubscriptRoutines duckdb_unresolved_type_subscript_routines;
extern const SubscriptRoutines duckdb_struct_subscript_routines;
extern const SubscriptRoutines duckdb_map_subscript_routines;

} // namespace pg
} // namespace pgddb
