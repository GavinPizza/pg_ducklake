#include "pgddb/pg/declarations.hpp"
#include "duckdb/common/enums/explain_format.hpp"

namespace pgddb::pg {
void ExplainPropertyText(const char *qlabel, const char *value, ExplainState *es);

duckdb::ExplainFormat DuckdbExplainFormat(ExplainState *es);

bool IsExplainAnalyze(ExplainState *es);
} // namespace pgddb::pg
