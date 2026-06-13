#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

struct ParamInfo {
	int param_id;
	Oid param_type;
	Oid element_type;
};

struct DirectInsertContext {
	Oid target_table_oid;
	uint64_t table_id;
	uint64_t schema_version;
	List *param_infos; // List of ParamInfo*
	int expected_row_count;
	List *target_col_names; // List of char*
	List *target_col_types; // List of Oid
};

PlannedStmt *TryCreateDirectInsertPlan(Query *parse, ParamListInfo bound_params);

/* Clear session-level caches.  Must be called on DuckDB instance recycle
 * (recycle_ddb) since table_id/schema_version may change. */
void ResetDirectInsertCaches();

/* Shared-memory outcome counters (pattern x reason).  Matched patterns always
 * pair with reason=ok.  Gated queries (non-INSERT / tx block / GUC off /
 * non-ducklake target) are filtered earlier and never bump counters. */
enum DirectInsertPattern {
	DI_PAT_MATCHED_UNNEST = 0,
	DI_PAT_MATCHED_VALUES,
	DI_PAT_UNMATCHED,
	DI_PAT_NUM,
};

enum DirectInsertReason {
	DI_R_OK = 0,
	DI_R_INVALID_RTE,
	DI_R_NO_INLINED_TABLE,
	DI_R_SCHEMA_VERSION_MISMATCH,
	DI_R_COL_TYPES_UNSUPPORTED,
	DI_R_GREATER_THAN_LIMIT,
	DI_R_UNSUPPORTED_INSERT_SHAPE,
	DI_R_RETRY,
	DI_R_NUM,
};

/* Bump counter; safe to call from any backend after ShmemStartup ran. */
void DirectInsertStatsBump(DirectInsertPattern pattern, DirectInsertReason reason);

void DirectInsertStatsReset();

uint64_t DirectInsertStatsRead(DirectInsertPattern pattern, DirectInsertReason reason);

/* Snapshot the whole matrix under one spinlock acquisition.  The
 * destination must be at least DI_PAT_NUM * DI_R_NUM uint64_t's. */
void DirectInsertStatsReadAll(uint64_t out[DI_PAT_NUM][DI_R_NUM]);

const char *DirectInsertPatternName(DirectInsertPattern pattern);
const char *DirectInsertReasonName(DirectInsertReason reason);

} // namespace pgducklake
