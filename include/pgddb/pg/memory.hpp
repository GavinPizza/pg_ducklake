#pragma once

#include "pgddb/pg/declarations.hpp"

namespace pgddb::pg {

MemoryContext MemoryContextCreate(MemoryContext parent, const char *name);
MemoryContext MemoryContextSwitchTo(MemoryContext target);
void MemoryContextReset(MemoryContext context);
void MemoryContextDelete(MemoryContext context);

} // namespace pgddb::pg
