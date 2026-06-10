#pragma once

extern "C" {
#include "postgres.h"
#include "nodes/extensible.h"
}

namespace pgddb {

extern CustomScanMethods scan_methods;
void InitNode(const char *custom_scan_name);

} // namespace pgddb
