#pragma once

#include "duckdb/function/function_set.hpp"

namespace pgducklake {

duckdb::TableFunctionSet GetTimeTravelFunctions();

} // namespace pgducklake
