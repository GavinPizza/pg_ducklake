#include "pg_vortex/vortex_hooks.hpp"
#include "pg_vortex/vortex_node.hpp"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress) {
		ereport(ERROR,
		        (errmsg("pg_vortex needs to be loaded via shared_preload_libraries"),
		         errhint("Add pg_vortex to shared_preload_libraries.")));
	}

	pg_vortex::InitNode();
	pg_vortex::InitHooks();
}

PG_FUNCTION_INFO_V1(vortex_version);
Datum
vortex_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text("pg_vortex 0.1.0"));
}

// Marker stub: any direct call to a duckdb-only function falls through here
// because the planner_hook should have offloaded the query to DuckDB. If we
// reach this body, the offload check missed (e.g., the function was called
// inside a context the walker doesn't traverse).
PG_FUNCTION_INFO_V1(duckdb_only_function);
Datum
duckdb_only_function(PG_FUNCTION_ARGS)
{
	char *function_name = DatumGetCString(DirectFunctionCall1(regprocout, fcinfo->flinfo->fn_oid));
	elog(ERROR, "Function '%s' only works with DuckDB execution", function_name);
}

} // extern "C"
