#pragma once

#include "duckdb.hpp"

#include "pgddb/pgddb_types.hpp"

extern "C" {
#include "postgres.h"
#include "common/int.h"
#include "executor/tuptable.h"
#include "utils/array.h"
}

namespace pgddb {

// Converts a duckdb LIST/ARRAY Value into a multi-dim PG ArrayType. The OP class
// supplies element-level Datum conversion and final ArrayType construction:
//
//   struct MyOP {
//       static Datum ConvertToPostgres(const duckdb::Value &val);
//       static ArrayType *ConstructArray(Datum *datums, bool *nulls,
//                                        int ndims, int *dims, int *lower_bound);
//   };

template <class OP>
struct PostgresArrayAppendState {
public:
	PostgresArrayAppendState(duckdb::idx_t _number_of_dimensions)
	    : expected_values(1), datums(nullptr), nulls(nullptr), dimensions(nullptr), lower_bounds(nullptr),
	      number_of_dimensions(_number_of_dimensions) {
		dimensions = (int *)palloc(number_of_dimensions * sizeof(int));
		lower_bounds = (int *)palloc(number_of_dimensions * sizeof(int));
		for (duckdb::idx_t i = 0; i < number_of_dimensions; i++) {
			dimensions[i] = -1; // -1 marks an unset dimension
		}
		for (duckdb::idx_t i = 0; i < number_of_dimensions; i++) {
			lower_bounds[i] = 1;
		}
	}

private:
	static inline const duckdb::vector<duckdb::Value> &
	GetChildren(const duckdb::Value &value) {
		switch (value.type().InternalType()) {
		case duckdb::PhysicalType::LIST:
			return duckdb::ListValue::GetChildren(value);
		case duckdb::PhysicalType::ARRAY:
			return duckdb::ArrayValue::GetChildren(value);
		default:
			throw duckdb::InvalidInputException("Expected a LIST or ARRAY type, got '%s' instead",
			                                    value.type().ToString());
		}
	}

public:
	void
	AppendValueAtDimension(const duckdb::Value &value, duckdb::idx_t dimension) {
		auto &values = GetChildren(value);

		if (values.size() > PG_INT32_MAX) {
			throw duckdb::InvalidInputException("Too many values (%llu) at dimension %d: would overflow", values.size(),
			                                    dimension);
		}

		int32_t to_append = values.size();

		D_ASSERT(dimension < number_of_dimensions);
		if (dimensions[dimension] == -1) {
			dimensions[dimension] = to_append;
			expected_values *= to_append;
			if (pg_mul_u64_overflow(expected_values, static_cast<uint64>(to_append), &expected_values)) {
				throw duckdb::InvalidInputException(
				    "Multiplying %d expected values by %d new ones at dimension %d would overflow", expected_values,
				    to_append, dimension);
			}
		}
		if (dimensions[dimension] != to_append) {
			throw duckdb::InvalidInputException("Expected %d values in list at dimension %d, found %d instead",
			                                    dimensions[dimension], dimension, to_append);
		}

		auto &child_type = GetChildType(value.type());
		if (child_type.id() == duckdb::LogicalTypeId::LIST) {
			for (auto &child_val : values) {
				if (child_val.IsNull()) {
					// PG arrays cannot hold a NULL at an intermediate dimension, e.g. {{1,2}, NULL, {3,4}}.
					throw duckdb::InvalidInputException("Returned LIST contains a NULL at an intermediate dimension "
					                                    "(not the value level), which is not supported in Postgres");
				}
				AppendValueAtDimension(child_val, dimension + 1);
			}
		} else {
			if (!datums) {
				// First leaf reached; all dimensions are known so expected_values is final.
				datums = (Datum *)palloc(expected_values * sizeof(Datum));
				nulls = (bool *)palloc(expected_values * sizeof(bool));
			}

			for (auto &child_val : values) {
				nulls[count] = child_val.IsNull();
				if (!nulls[count]) {
					datums[count] = OP::ConvertToPostgres(child_val);
				}
				++count;
			}
		}
	}

private:
	duckdb::idx_t count = 0;

public:
	uint64 expected_values = 1;
	Datum *datums = nullptr;
	bool *nulls = nullptr;
	int *dimensions;
	int *lower_bounds;
	duckdb::idx_t number_of_dimensions;
};

template <class OP>
void
ConvertDuckToPostgresArray(TupleTableSlot *slot, duckdb::Value &value, duckdb::idx_t col) {
	D_ASSERT(IsNestedType(value.type().id()));
	auto number_of_dimensions = [&]() {
		duckdb::idx_t depth = 0;
		const duckdb::LogicalType *t = &value.type();
		while (IsNestedType(t->id())) {
			t = &GetChildType(*t);
			depth++;
		}
		return depth;
	}();

	PostgresArrayAppendState<OP> append_state(number_of_dimensions);
	append_state.AppendValueAtDimension(value, 0);

	auto datums = append_state.datums;
	auto nulls = append_state.nulls;
	auto dimensions = append_state.dimensions;
	auto lower_bounds = append_state.lower_bounds;

	// An empty multi-dim array leaves inner dimensions unset (-1); treat them as 0.
	for (duckdb::idx_t i = 0; i < number_of_dimensions; i++) {
		if (dimensions[i] == -1) {
			dimensions[i] = 0;
		}
	}

	auto arr = OP::ConstructArray(datums, nulls, number_of_dimensions, dimensions, lower_bounds);

	if (append_state.expected_values > 0) {
		pfree(datums);
		pfree(nulls);
	}
	pfree(dimensions);
	pfree(lower_bounds);

	slot->tts_values[col] = PointerGetDatum(arr);
}

} // namespace pgddb
