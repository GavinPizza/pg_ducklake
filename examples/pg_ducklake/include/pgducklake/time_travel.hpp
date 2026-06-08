#pragma once

/*
 * time_travel.hpp -- Time-travel query support for DuckLake tables
 *
 * Declares the DuckDB table function `time_travel(table_name, version/timestamp)`
 * that enables querying DuckLake tables at historical snapshots.
 */

#include "duckdb/function/function_set.hpp"

namespace pgducklake {

duckdb::TableFunctionSet GetTimeTravelFunctions();

} // namespace pgducklake
