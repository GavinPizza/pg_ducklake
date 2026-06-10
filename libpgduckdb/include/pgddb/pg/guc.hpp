#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgddb::pg {
extern const char *GetConfigOption(const char *name, bool missing_ok = false, bool restrict_privileged = true);
}
