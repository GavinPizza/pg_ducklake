#pragma once

#include "string"
#include "pgddb/pg/declarations.hpp"

namespace pgddb::pg {

std::string GetArgString(FunctionCallInfo info, int argno);
Datum GetArgDatum(FunctionCallInfo info, int argno);
std::string DatumToString(Datum datum);

} // namespace pgddb::pg
