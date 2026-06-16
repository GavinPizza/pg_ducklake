#pragma once

#include "pgddb/pg/declarations.hpp"

#include <vector>

#include "pgddb/utility/cpp_only_file.hpp" // Must be last include.

namespace pgddb {

class PostgresTableReader {
public:
	PostgresTableReader();
	~PostgresTableReader();
	void Init(const char *table_scan_query, bool count_tuples_only);
	void Cleanup();
	bool GetNextMinimalWorkerTuple(std::vector<uint8_t> &minimal_tuple_buffer);
	// In-process (no workers): each slot owns its copy (freed on next store); result < max means EOF. Caller holds
	// GlobalProcessLock.
	int GetNextInProcessTuples(TupleTableSlot **slots, int max);
	// count_tuples_only: sets *count_out to the partial count; false on EOF.
	bool GetNextCount(uint64_t *count_out);
	TupleTableSlot *InitTupleSlot();
	int
	NumWorkersLaunched() const {
		return nworkers_launched;
	}

private:
	PostgresTableReader(const PostgresTableReader &) = delete;
	PostgresTableReader &operator=(const PostgresTableReader &) = delete;

	void InitUnsafe(const char *table_scan_query, bool count_tuples_only);
	void InitRunWithParallelScan(PlannedStmt *, bool);
	void CleanupUnsafe();

	TupleTableSlot *GetNextTuple();
	int GetNextInProcessTuplesUnsafe(TupleTableSlot **slots, int max);
	TupleTableSlot *GetNextTupleUnsafe();
	TupleTableSlot *ExecNextTupleUnsafe();
	MinimalTuple GetNextWorkerTuple();
	int ParallelWorkerNumber(Cardinality cardinality);
	bool CanTableScanRunInParallel(Plan *plan);
	bool MarkPlanParallelAware(Plan *plan);

	QueryDesc *table_scan_query_desc;
	PlanState *table_scan_planstate;
	ParallelExecutorInfo *parallel_executor_info;
	void **parallel_worker_readers;
	TupleTableSlot *slot;
	int nworkers_launched;
	int nreaders;
	int next_parallel_reader;
	bool entered_parallel_mode;
	bool cleaned_up;
};

// Logs the EXPLAIN plan of a Postgres scan at the NOTICE log level
extern bool duckdb_log_pg_explain;
// Maximum number of PostgreSQL workers used for a single Postgres scan
extern int duckdb_max_workers_per_postgres_scan;

} // namespace pgddb
