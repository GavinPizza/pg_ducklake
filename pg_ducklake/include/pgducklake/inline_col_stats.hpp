#pragma once

#include "pgddb/pg/declarations.hpp"
#include "pgducklake/pgducklake_metadata_manager.hpp"

#include <memory>
#include <vector>

namespace pgducklake {

/* Per-column min/max + null accumulator for a single inline-insert batch (direct insert or COPY
 * FROM).
 * Cells are folded as native PG Datums, mirroring DuckLakeColumnStats::MergeStats without a
 * per-cell text->Value->cast->ToString round-trip. Only the two surviving bounds per column are
 * canonicalized (via duckdb::Value::ToString) at Finalize(), so the persisted encoding round-trips
 * through the read path exactly as native DuckLake's inline write would produce it.
 * CreateSnapshotForDirectInsert widens those bounds into ducklake_table_column_stats.
 * The DuckDB-typed and PG-typed state lives behind a pimpl so this header stays free of DuckDB and
 * postgres.h includes. */
class InlineColStats {
public:
	/* Fetches column_id + DuckLake type for the first num_cols user columns (ordered by
	 * column_order -- both the direct-insert and COPY paths write a prefix of the table columns
	 * in that order). MUST be called inside an already-open SPI connection. On any metadata
	 * shortfall the accumulator is inactive and no stats are maintained (existing bounds stay
	 * intact -- never widened into a lie). */
	InlineColStats(uint64_t table_id, int num_cols);
	~InlineColStats();

	InlineColStats(const InlineColStats &) = delete;
	InlineColStats &operator=(const InlineColStats &) = delete;

	bool ColumnEligible(int col) const;
	/* Bind an eligible column's PG source type so cells can be compared natively. Call once per
	 * column before the row loop; a no-op for ineligible columns. If the source type has no usable
	 * comparison the column is demoted to ineligible. Needs no SPI connection. */
	void SetupColumn(int col, Oid pg_source_type);
	void ObserveDatum(int col, Datum value);
	/* Not gated on ColumnEligible: null-ness needs no comparison proc or canonical encoding, so it
	 * costs nothing to match the normal path here. The min/max exclusions are not a precedent --
	 * they exist only because a canonical bound for those types is not cheap to produce. */
	void ObserveNull(int col);
	void Finalize(std::vector<DirectInsertColumnStat> &out);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace pgducklake
