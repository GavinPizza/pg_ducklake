#pragma once

struct Query;

namespace pgducklake {
void RegisterForeignTablesInQuery(Query *query);
bool QueryReferencesDucklakeForeignTable(Query *query);
} // namespace pgducklake
