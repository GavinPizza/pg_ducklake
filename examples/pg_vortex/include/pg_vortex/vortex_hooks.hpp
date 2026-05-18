#pragma once

namespace pg_vortex {

// Installs pg_vortex's planner_hook and ExplainOneQuery_hook. Call from
// _PG_init after vortex_node::InitNode().
void InitHooks();

} // namespace pg_vortex
