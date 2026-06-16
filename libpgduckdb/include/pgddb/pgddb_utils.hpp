#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "pgddb/pgddb_duckdb.hpp"
#include "pgddb/pg/error_data.hpp"
#include "pgddb/logger.hpp"

#include <setjmp.h>

#include "pgddb/utility/cpp_only_file.hpp" // Must be last include.

extern "C" {
struct ErrorContextCallback;
struct MemoryContextData;

typedef struct MemoryContextData *MemoryContext;
typedef char *pg_stack_base_t;

extern sigjmp_buf *PG_exception_stack;
extern MemoryContext CurrentMemoryContext;
extern ErrorContextCallback *error_context_stack;
extern ErrorData *CopyErrorData();
extern void FlushErrorState();
extern pg_stack_base_t set_stack_base();
extern void restore_stack_base(pg_stack_base_t base);
}

namespace pgddb {

struct PgExceptionGuard {
	PgExceptionGuard() : _save_exception_stack(PG_exception_stack), _save_context_stack(error_context_stack) {
	}

	~PgExceptionGuard() noexcept {
		PG_exception_stack = _save_exception_stack;
		error_context_stack = _save_context_stack;
	}

	sigjmp_buf *_save_exception_stack;
	ErrorContextCallback *_save_context_stack;

private:
	PgExceptionGuard(const PgExceptionGuard &) = delete;
	PgExceptionGuard &operator=(const PgExceptionGuard &) = delete;
};

/*
 * RAII reset of PG's saved stack base. PG's max_stack_depth check compares the
 * current stack pointer against the base captured at process start; on a
 * non-main thread (different stack) that check spuriously fails, so we rebase
 * it to the current location for the duration of the call.
 */
struct PostgresScopedStackReset {
	PostgresScopedStackReset() : saved_current_stack(set_stack_base()) {
	}

	~PostgresScopedStackReset() {
		restore_stack_base(saved_current_stack);
	}
	pg_stack_base_t saved_current_stack;

private:
	PostgresScopedStackReset(const PostgresScopedStackReset &) = delete;
	PostgresScopedStackReset &operator=(const PostgresScopedStackReset &) = delete;
};

// DuckdbGlobalLock should be held before calling.
template <typename Func, Func func, typename... FuncArgs>
typename std::invoke_result<Func, FuncArgs...>::type
__PostgresFunctionGuard__(const char *func_name, FuncArgs... args) {
	std::lock_guard<std::recursive_mutex> lock(pgddb::GlobalProcessLock::GetLock());
	MemoryContext ctx = CurrentMemoryContext;

	{ // PG_TRY
		PgExceptionGuard g;
		sigjmp_buf _local_sigjmp_buf;
		if (sigsetjmp(_local_sigjmp_buf, 0) == 0) {
			PG_exception_stack = &_local_sigjmp_buf;
			return func(std::forward<FuncArgs>(args)...);
		}
	}

	CurrentMemoryContext = ctx;

	ErrorData *edata = nullptr;
	{ // PG_CATCH
		PgExceptionGuard g;
		sigjmp_buf _local_sigjmp_buf;
		if (sigsetjmp(_local_sigjmp_buf, 0) == 0) {
			PG_exception_stack = &_local_sigjmp_buf;

			edata = CopyErrorData();
			FlushErrorState();
		} else {
			throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, "Failed to extract Postgres error message");
		}
	} // PG_END_TRY

	auto message = duckdb::StringUtil::Format("(PGDuckDB/%s) %s", func_name, pgddb::pg::GetErrorDataMessage(edata));
	throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, message);
}

#define PostgresFunctionGuard(FUNC, ...) pgddb::__PostgresFunctionGuard__<decltype(&FUNC), &FUNC>(#FUNC, ##__VA_ARGS__)

template <typename T, typename ReturnType, typename... FuncArgs>
ReturnType
__PostgresMemberGuard__(ReturnType (T::*func)(FuncArgs... args), T *instance, const char *func_name, FuncArgs... args) {
	MemoryContext ctx = CurrentMemoryContext;

	{ // Scope for PG_END_TRY
		PgExceptionGuard g;
		sigjmp_buf _local_sigjmp_buf;
		if (sigsetjmp(_local_sigjmp_buf, 0) == 0) {
			PG_exception_stack = &_local_sigjmp_buf;
			return (instance->*func)(std::forward<FuncArgs>(args)...);
		}
	} // PG_END_TRY();

	CurrentMemoryContext = ctx;

	ErrorData *edata = nullptr;

	{ // PG_CATCH
		PgExceptionGuard g;
		sigjmp_buf _local_sigjmp_buf;
		if (sigsetjmp(_local_sigjmp_buf, 0) == 0) {
			PG_exception_stack = &_local_sigjmp_buf;

			edata = CopyErrorData();
			FlushErrorState();
		} else {
			throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, "Failed to extract Postgres error message");
		}
	} // PG_END_TRY

	auto message = duckdb::StringUtil::Format("(PGDuckDB/%s) %s", func_name, pgddb::pg::GetErrorDataMessage(edata));
	throw duckdb::Exception(duckdb::ExceptionType::EXECUTOR, message);
}

#define PostgresMemberGuard(FUNC, ...) pgddb::__PostgresMemberGuard__(&FUNC, this, __func__, ##__VA_ARGS__)

void AppendEscapedUri(std::ostringstream &oss, const char *str);

} // namespace pgddb
