// pg_vortex's CustomScan node. Copied + adapted from
// examples/pg_duckdb/src/pgduckdb_node.cpp. Differences:
// - CustomScanMethods/ExecMethods tagged "VortexScan" (must be globally
//   unique when both pg_duckdb and pg_vortex load in the same backend).
// - References pg_vortex::Prepare instead of DuckdbPrepare.
// - ContainsPostgresTable inlined locally (pg_vortex's check is simpler: no
//   IsDuckdbTable filter; any RTE with a valid relid is treated as a
//   non-vortex table, which forces materialized results).
// - EXPLAIN-globals (duckdb_explain_analyze etc.) kept as plain globals at
//   namespace pg_vortex; pg_vortex's _PG_init wires its own
//   ExplainOneQuery_hook to set them (B7).

#include "duckdb.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/exception.hpp"

#include "pg_vortex/vortex_planner.hpp"
#include "pgddb/pgddb_types.hpp"
#include "pgddb/vendor/pg_explain.hpp"
#include "pgddb/pg/explain.hpp"

extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "catalog/pg_class.h"
#include "tcop/pquery.h"
#include "nodes/params.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
}

#include "pg_vortex/vortex_node.hpp"
#include "pgddb/pgddb_duckdb.hpp"
#include "pgddb/utility/cpp_wrapper.hpp"

namespace pg_vortex {

// EXPLAIN-time flags. Set by pg_vortex's ExplainOneQuery hook (B7) before
// the planner runs; consumed by Explain callback below.
bool vortex_explain_analyze = false;
duckdb::ExplainFormat vortex_explain_format = duckdb::ExplainFormat::DEFAULT;

#define NEED_JSON_PLAN(explain_format) (explain_format == duckdb::ExplainFormat::JSON)

CustomScanMethods vortex_scan_scan_methods;
static CustomExecMethods vortex_scan_exec_methods;

typedef struct VortexScanState {
	CustomScanState css; // must be first field
	const CustomScan *custom_scan;
	const Query *query;
	ParamListInfo params;
	duckdb::Connection *duckdb_connection;
	duckdb::PreparedStatement *prepared_statement;
	bool is_executed;
	bool fetch_next;
	duckdb::unique_ptr<duckdb::QueryResult> query_results;
	duckdb::idx_t column_count;
	duckdb::unique_ptr<duckdb::DataChunk> current_data_chunk;
	duckdb::idx_t current_row;
} VortexScanState;

static bool
ContainsPostgresTable(Node *node, void * /*context*/) {
	if (node == NULL)
		return false;

	if (IsA(node, Query)) {
		Query *query = (Query *)node;
		List *rtable = query->rtable;
		foreach_node(RangeTblEntry, rte, rtable) {
			if (rte->relid == InvalidOid) {
				// Function RTEs (read_vortex) have invalid relid -- skip.
				continue;
			}
			char relkind = get_rel_relkind(rte->relid);
			if (relkind == RELKIND_VIEW) {
				continue;
			}
			// Any other RTE with a valid relid is a real PG table.
			return true;
		}
#if PG_VERSION_NUM >= 160000
		return query_tree_walker(query, ContainsPostgresTable, NULL, 0);
#else
		return query_tree_walker(query, (bool (*)())((void *)ContainsPostgresTable), NULL, 0);
#endif
	}

#if PG_VERSION_NUM >= 160000
	return expression_tree_walker(node, ContainsPostgresTable, NULL);
#else
	return expression_tree_walker(node, (bool (*)())((void *)ContainsPostgresTable), NULL);
#endif
}

static void
CleanupVortexScanState(VortexScanState *state) {
	MemoryContextReset(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	ExecClearTuple(state->css.ss.ss_ScanTupleSlot);

	state->query_results.reset();
	state->current_data_chunk.reset();

	if (state->prepared_statement) {
		delete state->prepared_statement;
		state->prepared_statement = nullptr;
	}
}

static Node *Vortex_CreateCustomScanState(CustomScan *cscan);
static void Vortex_BeginCustomScan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *Vortex_ExecCustomScan(CustomScanState *node);
static void Vortex_EndCustomScan(CustomScanState *node);
static void Vortex_ReScanCustomScan(CustomScanState *node);
static void Vortex_ExplainCustomScan(CustomScanState *node, List *ancestors, ExplainState *es);
static inline void formatDuckDbPlanForPG(const char *duckdb_plan, ExplainState *es);

static Node *
Vortex_CreateCustomScanState(CustomScan *cscan) {
	VortexScanState *state = (VortexScanState *)newNode(sizeof(VortexScanState), T_CustomScanState);
	CustomScanState *custom_scan_state = &state->css;
	state->custom_scan = cscan;
	state->query = (const Query *)linitial(cscan->custom_private);
	custom_scan_state->methods = &vortex_scan_exec_methods;
	return (Node *)custom_scan_state;
}

static void
Vortex_BeginCustomScan_Cpp(CustomScanState *cscanstate, EState *estate, int /*eflags*/) {
	VortexScanState *state = (VortexScanState *)cscanstate;

	StringInfo explain_prefix = makeStringInfo();
	bool is_explain_query = ActivePortal && ActivePortal->commandTag == CMDTAG_EXPLAIN;

	if (is_explain_query) {
		appendStringInfoString(explain_prefix, "EXPLAIN ");
		if (NEED_JSON_PLAN(vortex_explain_format))
			appendStringInfoChar(explain_prefix, '(');
		if (vortex_explain_analyze) {
			if (NEED_JSON_PLAN(vortex_explain_format))
				appendStringInfoString(explain_prefix, "ANALYZE, ");
			else
				appendStringInfoString(explain_prefix, "ANALYZE ");
		}
		if (NEED_JSON_PLAN(vortex_explain_format)) {
			appendStringInfoString(explain_prefix, "FORMAT JSON )");
		}
	}

	duckdb::unique_ptr<duckdb::PreparedStatement> prepared_query =
	    pg_vortex::Prepare(state->query, explain_prefix->data);

	if (prepared_query->HasError()) {
		throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR,
		                        "DuckDB re-planning failed: " + prepared_query->GetError());
	}

	if (!is_explain_query) {
		auto &prepared_result_types = prepared_query->GetTypes();
		size_t target_list_length = static_cast<size_t>(list_length(state->custom_scan->custom_scan_tlist));
		if (prepared_result_types.size() != target_list_length) {
			elog(ERROR,
			     "(pg_vortex/BeginCustomScan) Number of columns returned by DuckDB query changed between planning "
			     "and execution, expected %zu got %zu",
			     target_list_length, prepared_result_types.size());
		}
		for (size_t i = 0; i < prepared_result_types.size(); i++) {
			Oid postgres_column_oid = pgddb::GetPostgresDuckDBType(prepared_result_types[i], true);
			TargetEntry *target_entry =
			    list_nth_node(TargetEntry, state->custom_scan->custom_scan_tlist, i);
			Var *var = castNode(Var, target_entry->expr);
			if (var->vartype != postgres_column_oid) {
				elog(ERROR,
				     "Types returned by duckdb query changed between planning and execution, expected %d got %d",
				     var->vartype, postgres_column_oid);
			}
		}
	}

	state->duckdb_connection = pgddb::DuckDBManager::GetConnection();
	state->prepared_statement = prepared_query.release();
	state->params = estate->es_param_list_info;
	state->is_executed = false;
	state->fetch_next = true;
	state->css.ss.ps.ps_ResultTupleDesc = state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	HOLD_CANCEL_INTERRUPTS();
}

static void
Vortex_BeginCustomScan(CustomScanState *cscanstate, EState *estate, int eflags) {
	InvokeCPPFunc(Vortex_BeginCustomScan_Cpp, cscanstate, estate, eflags);
}

static void
ExecuteQuery(VortexScanState *state) {
	auto &prepared = *state->prepared_statement;
	auto pg_params = state->params;
	const auto num_params = pg_params ? pg_params->numParams : 0;
	duckdb::case_insensitive_map_t<duckdb::BoundParameterData> named_values;

	for (int i = 0; i < num_params; i++) {
		ParamExternData *pg_param;
		ParamExternData tmp_workspace;
		duckdb::Value duckdb_param;

		if (pg_params->paramFetch != NULL) {
			pg_param = pg_params->paramFetch(pg_params, i + 1, false, &tmp_workspace);
		} else {
			pg_param = &pg_params->params[i];
		}

		if (prepared.named_param_map.count(duckdb::to_string(i + 1)) == 0) {
			continue;
		}

		if (pg_param->isnull) {
			duckdb_param = duckdb::Value();
		} else if (OidIsValid(pg_param->ptype)) {
			duckdb_param = pgddb::ConvertPostgresParameterToDuckValue(pg_param->value, pg_param->ptype);
		} else {
			std::ostringstream oss;
			oss << "parameter '" << i << "' has an invalid type (" << pg_param->ptype << ") during query execution";
			throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, oss.str().c_str());
		}
		named_values[duckdb::to_string(i + 1)] = duckdb::BoundParameterData(duckdb_param);
	}

	bool allow_stream_result = !ContainsPostgresTable((Node *)state->query, NULL);
	auto pending = prepared.PendingQuery(named_values, allow_stream_result);
	if (pending->HasError()) {
		return pending->ThrowError();
	}

	duckdb::PendingExecutionResult execution_result = duckdb::PendingExecutionResult::RESULT_NOT_READY;
	while (true) {
		execution_result = pending->ExecuteTask();
		if (duckdb::PendingQueryResult::IsResultReady(execution_result)) {
			break;
		}
		if (QueryCancelPending) {
			auto &connection = state->duckdb_connection;
			connection->Interrupt();
			auto &executor = duckdb::Executor::Get(*connection->context);
			executor.CancelTasks();
			try {
				do {
					execution_result = pending->ExecuteTask();
				} while (execution_result != duckdb::PendingExecutionResult::EXECUTION_ERROR &&
				         execution_result != duckdb::PendingExecutionResult::NO_TASKS_AVAILABLE &&
				         execution_result != duckdb::PendingExecutionResult::EXECUTION_FINISHED);
				pending->Close();
			} catch (std::exception &) {
			}
			ProcessInterrupts();
			throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, "Query cancelled");
		}
	}

	if (execution_result == duckdb::PendingExecutionResult::EXECUTION_ERROR) {
		return pending->ThrowError();
	}

	state->query_results = pending->Execute();
	state->column_count = state->query_results->ColumnCount();
	state->is_executed = true;
}

static TupleTableSlot *
Vortex_ExecCustomScan_Cpp(CustomScanState *node) {
	VortexScanState *state = (VortexScanState *)node;
	try {
		TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
		MemoryContext old_context;

		if (ActivePortal && ActivePortal->commandTag == CMDTAG_EXPLAIN) {
			ExecClearTuple(slot);
			return slot;
		}

		bool already_executed = state->is_executed;
		if (!already_executed) {
			ExecuteQuery(state);
		}

		if (state->fetch_next) {
			state->current_data_chunk = state->query_results->Fetch();
			state->current_row = 0;
			state->fetch_next = false;
			if (!state->current_data_chunk || state->current_data_chunk->size() == 0) {
				MemoryContextReset(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
				ExecClearTuple(slot);
				return slot;
			}
		}

		MemoryContextReset(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
		ExecClearTuple(slot);

		old_context = MemoryContextSwitchTo(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

		for (idx_t col = 0; col < state->column_count; col++) {
			auto value = state->current_data_chunk->GetValue(col, state->current_row);
			if (value.IsNull()) {
				slot->tts_isnull[col] = true;
			} else {
				slot->tts_isnull[col] = false;
				if (!pgddb::ConvertDuckToPostgresValue(slot, value, col)) {
					throw duckdb::ConversionException("Value conversion failed");
				}
			}
		}

		MemoryContextSwitchTo(old_context);

		state->current_row++;
		if (state->current_row >= state->current_data_chunk->size()) {
			state->current_data_chunk.reset();
			state->fetch_next = true;
		}

		ExecStoreVirtualTuple(slot);
		return slot;
	} catch (std::exception &) {
		CleanupVortexScanState(state);
		throw;
	}
}

static TupleTableSlot *
Vortex_ExecCustomScan(CustomScanState *node) {
	return InvokeCPPFunc(Vortex_ExecCustomScan_Cpp, node);
}

static void
Vortex_EndCustomScan_Cpp(CustomScanState *node) {
	VortexScanState *state = (VortexScanState *)node;
	CleanupVortexScanState(state);
#ifdef USE_ASSERT_CHECKING
	RESUME_CANCEL_INTERRUPTS();
#else
	if (QueryCancelHoldoffCount > 0) {
		RESUME_CANCEL_INTERRUPTS();
	}
#endif
}

static void
Vortex_EndCustomScan(CustomScanState *node) {
	InvokeCPPFunc(Vortex_EndCustomScan_Cpp, node);
}

static void
Vortex_ReScanCustomScan(CustomScanState * /*node*/) {
}

static void
Vortex_ExplainCustomScan_Cpp(CustomScanState *node, ExplainState *es) {
	vortex_explain_analyze = pgddb::pg::IsExplainAnalyze(es);
	vortex_explain_format = pgddb::pg::DuckdbExplainFormat(es);

	VortexScanState *state = (VortexScanState *)node;
	ExecuteQuery(state);

	auto chunk = state->query_results->Fetch();
	if (!chunk || chunk->size() == 0) {
		return;
	}

	auto value = chunk->GetValue(1, 0).GetValue<duckdb::string>();

	do {
		chunk = state->query_results->Fetch();
	} while (chunk && chunk->size() > 0);

	std::ostringstream explain_output;
	explain_output << "\n\n" << value << "\n";
	if (NEED_JSON_PLAN(vortex_explain_format)) {
		if (linitial_int(es->grouping_stack) != 0)
			appendStringInfoChar(es->str, ',');
		else
			linitial_int(es->grouping_stack) = 1;
		appendStringInfoChar(es->str, '\n');
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfoString(es->str, "\"DuckDB Execution Plan\": ");
		formatDuckDbPlanForPG(value.c_str(), es);
	} else {
		pgddb::pg::ExplainPropertyText("DuckDB Execution Plan", explain_output.str().c_str(), es);
	}
}

static inline void
formatDuckDbPlanForPG(const char *duckdb_plan, ExplainState *es) {
	const char *ptr = duckdb_plan;
	while (*ptr != '\0') {
		appendStringInfoChar(es->str, *ptr);
		if (*ptr == '\n') {
			appendStringInfoSpaces(es->str, es->indent * 2);
		}
		ptr++;
	}
}

static void
Vortex_ExplainCustomScan(CustomScanState *node, List * /*ancestors*/, ExplainState *es) {
	InvokeCPPFunc(Vortex_ExplainCustomScan_Cpp, node, es);
}

void
InitNode() {
	memset(&vortex_scan_scan_methods, 0, sizeof(vortex_scan_scan_methods));
	vortex_scan_scan_methods.CustomName = "VortexScan";
	vortex_scan_scan_methods.CreateCustomScanState = Vortex_CreateCustomScanState;
	RegisterCustomScanMethods(&vortex_scan_scan_methods);

	memset(&vortex_scan_exec_methods, 0, sizeof(vortex_scan_exec_methods));
	vortex_scan_exec_methods.CustomName = "VortexScan";

	vortex_scan_exec_methods.BeginCustomScan = Vortex_BeginCustomScan;
	vortex_scan_exec_methods.ExecCustomScan = Vortex_ExecCustomScan;
	vortex_scan_exec_methods.EndCustomScan = Vortex_EndCustomScan;
	vortex_scan_exec_methods.ReScanCustomScan = Vortex_ReScanCustomScan;
	vortex_scan_exec_methods.EstimateDSMCustomScan = NULL;
	vortex_scan_exec_methods.InitializeDSMCustomScan = NULL;
	vortex_scan_exec_methods.ReInitializeDSMCustomScan = NULL;
	vortex_scan_exec_methods.InitializeWorkerCustomScan = NULL;
	vortex_scan_exec_methods.ShutdownCustomScan = NULL;
	vortex_scan_exec_methods.ExplainCustomScan = Vortex_ExplainCustomScan;
}

} // namespace pg_vortex
