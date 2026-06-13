#pragma once

#include "duckdb.hpp"

extern "C" {
#include "postgres.h"
#include "nodes/extensible.h"
}

namespace pg_vortex {

extern CustomScanMethods vortex_scan_scan_methods;

extern bool vortex_explain_analyze;
extern duckdb::ExplainFormat vortex_explain_format;

void InitNode();

} // namespace pg_vortex
