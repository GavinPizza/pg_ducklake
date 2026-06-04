#pragma once

/*
 * pgducklake_maintenance.hpp -- DuckLake background maintenance worker.
 *
 * Provides a launcher/worker architecture for periodic table maintenance
 * (flush inlined data, rewrite data files, merge files, expire snapshots,
 * clean up old files).
 */

/* Hard cap on concurrent maintenance workers (GUC max bound) */
#define DUCKLAKE_MAX_MAINTENANCE_WORKERS 8
