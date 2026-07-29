#include "pgducklake/inline_col_stats.hpp"

#include <string>

#include <duckdb.hpp>

#include <common/ducklake_types.hpp>
#include <storage/ducklake_stats.hpp>

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
}

namespace pgducklake {

namespace {

struct ColStatEntry {
	uint64_t column_id = 0;
	duckdb::LogicalType type = duckdb::LogicalType::SQLNULL;
	bool type_known = false;
	bool eligible = false;
	bool value_cmp = false;       /* RequiresValueComparison(type) */
	bool finalize_direct = false; /* PG output text is already the DuckDB-canonical encoding */
	bool ready = false;           /* SetupColumn bound a usable PG comparison */
	bool has = false;

	/* PG source-type machinery bound by SetupColumn. */
	Oid src_type = InvalidOid;
	FmgrInfo out_finfo;             /* output function for src_type (Finalize / lexicographic loop) */
	FmgrInfo *cmp_finfo = nullptr;  /* value_cmp: btree comparison proc (owned by the type cache) */
	Oid cmp_collation = InvalidOid; /* value_cmp eligible types are non-collatable */
	int16 typlen = 0;
	bool typbyval = false;

	/* value_cmp path: running min/max kept as copied Datums in the accumulator context. */
	Datum min_datum = 0;
	Datum max_datum = 0;
	/* lexicographic (non-value_cmp) path: running min/max kept as canonical text. */
	std::string min_str;
	std::string max_str;
	uint64_t null_count = 0;
};

/* True for value_cmp types whose PG output text is already byte-for-byte the DuckDB-canonical
 * encoding, so Finalize can store the raw OutputFunctionCall text and skip the
 * Value(text)->cast(type)->ToString round-trip.
 *
 * Restricted to the integer family (signed and unsigned, incl. HUGEINT/UHUGEINT). Proof of
 * equality: an integer-typed DuckLake column only ever holds integral values, and the PG source
 * type is either an int2/int4/int8 (int*out) or -- for the wider types stored as VARCHAR --
 * numeric (numeric_out of an integral value). All of those emit a plain base-10 string: an
 * optional leading '-' then digits, no leading zeros, no thousands separators, no decimal point,
 * never scientific notation. DuckDB's ToString for every integer type emits exactly the same plain
 * base-10 string. So Value(pg_text).Cast(int_type).ToString() == pg_text for all values.
 *
 * Deliberately excluded (kept on the full round-trip):
 *   - BOOLEAN: PG emits 't'/'f', DuckDB canonical is 'true'/'false'.
 *   - DECIMAL: scale/zero/sign formatting equivalence between numeric_out and DuckDB decimal
 *     ToString is not proven here.
 *   - DATE/TIME/TIMESTAMP*: PG ISO output and DuckDB ToString agree in the common era but can
 *     diverge for BC / out-of-range years (e.g. PG '... BC', 5+ digit years). Not proven, so not
 *     guessed. */
bool
CanonicalEqualsOutput(const duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::TINYINT:
	case duckdb::LogicalTypeId::SMALLINT:
	case duckdb::LogicalTypeId::INTEGER:
	case duckdb::LogicalTypeId::BIGINT:
	case duckdb::LogicalTypeId::HUGEINT:
	case duckdb::LogicalTypeId::UTINYINT:
	case duckdb::LogicalTypeId::USMALLINT:
	case duckdb::LogicalTypeId::UINTEGER:
	case duckdb::LogicalTypeId::UBIGINT:
	case duckdb::LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

/* Types for which native DuckLake persists a table-level min/max we can safely maintain from
 * inlined tuples. Mirrors DuckLakeColumnStats::ToStats() minus FLOAT/DOUBLE (NaN handling) and
 * GEOMETRY/VARIANT/BLOB (extra_stats / no stats; never reach the inline path anyway).
 *
 * Gates min/max only -- ObserveNull is deliberately not gated by it.
 *
 * Excluding FLOAT/DOUBLE means their persisted bounds go stale rather than being widened, which is
 * safe only while those bounds are never turned into read-side statistics: ToStats() returns nullptr
 * for FLOAT/DOUBLE unless has_contains_nan && !contains_nan, and contains_nan is NULL on every table
 * this path can reach. That is an invariant held elsewhere, not here -- see the
 * "FIXME: we can gather nan statistics for FLOAT/DOUBLE" in
 * third_party/ducklake/src/storage/ducklake_inline_data.cpp. If that FIXME lands, a stale FLOAT
 * bound becomes a live mis-pruning bug and these two types must be added here (or excluded on the
 * read side) at the same time. */
bool
StatsEligible(const duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::BOOLEAN:
	case duckdb::LogicalTypeId::TINYINT:
	case duckdb::LogicalTypeId::SMALLINT:
	case duckdb::LogicalTypeId::INTEGER:
	case duckdb::LogicalTypeId::BIGINT:
	case duckdb::LogicalTypeId::HUGEINT:
	case duckdb::LogicalTypeId::UTINYINT:
	case duckdb::LogicalTypeId::USMALLINT:
	case duckdb::LogicalTypeId::UINTEGER:
	case duckdb::LogicalTypeId::UBIGINT:
	case duckdb::LogicalTypeId::UHUGEINT:
	case duckdb::LogicalTypeId::DECIMAL:
	case duckdb::LogicalTypeId::DATE:
	case duckdb::LogicalTypeId::TIME:
	case duckdb::LogicalTypeId::TIMESTAMP:
	case duckdb::LogicalTypeId::TIMESTAMP_TZ:
	case duckdb::LogicalTypeId::TIMESTAMP_SEC:
	case duckdb::LogicalTypeId::TIMESTAMP_MS:
	case duckdb::LogicalTypeId::TIMESTAMP_NS:
	case duckdb::LogicalTypeId::UUID:
	case duckdb::LogicalTypeId::VARCHAR:
		return true;
	default:
		return false;
	}
}

/* Forces ISO/YMD temporal output for its scope. Restoring from a destructor keeps a throw out of
 * the canonicalization path from leaking the forced setting into the rest of the session. A PG
 * elog(ERROR) longjmp still bypasses it, and DateStyle is assigned directly rather than through the
 * GUC machinery, so transaction abort would not undo it either. */
struct IsoDateStyleGuard {
	const int saved_style = DateStyle;
	const int saved_order = DateOrder;

	IsoDateStyleGuard() {
		DateStyle = USE_ISO_DATES;
		DateOrder = DATEORDER_YMD;
	}
	~IsoDateStyleGuard() {
		DateStyle = saved_style;
		DateOrder = saved_order;
	}
};

} // namespace

struct InlineColStats::Impl {
	std::vector<ColStatEntry> entries;
	bool active = false;
	/* Long-lived home for copied min/max Datums and cached output FmgrInfos: outlives the per-tuple
	 * / per-batch contexts the callers reset mid-loop. Parented on the transaction rather than
	 * TopMemoryContext because a PG elog(ERROR) longjmps past ~Impl -- under TopMemoryContext the
	 * context would then survive to backend exit, one leak per failed insert. This makes abort free
	 * it. Requires that no caller outlive the (sub)transaction current at construction. */
	MemoryContext ctx = nullptr;

	Impl() {
		ctx = AllocSetContextCreate(CurTransactionContext, "InlineColStats", ALLOCSET_SMALL_SIZES);
	}
	~Impl() {
		if (ctx) {
			MemoryContextDelete(ctx);
		}
	}
};

InlineColStats::InlineColStats(uint64_t table_id, int num_cols) : impl(new Impl()) {
	StringInfoData q;
	initStringInfo(&q);
	appendStringInfo(&q, R"(
		SELECT column_id, column_type FROM ducklake.ducklake_column
		WHERE table_id = %llu AND end_snapshot IS NULL AND parent_column IS NULL
		ORDER BY column_order)",
	                 (unsigned long long)table_id);
	int ret = SPI_execute(q.data, true, 0);
	if (ret != SPI_OK_SELECT || (int)SPI_processed < num_cols) {
		return;
	}
	impl->entries.resize(num_cols);
	for (int i = 0; i < num_cols; i++) {
		bool isnull;
		Datum id_d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull) {
			impl->entries.clear();
			return;
		}
		impl->entries[i].column_id = (uint64_t)DatumGetInt64(id_d);
		char *type_str = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);
		if (!type_str) {
			continue;
		}
		try {
			duckdb::LogicalType t = duckdb::DuckLakeTypes::FromString(type_str);
			/* A null-only stat still goes through the typed merge. */
			impl->entries[i].type = t;
			impl->entries[i].type_known = true;
			if (StatsEligible(t)) {
				impl->entries[i].eligible = true;
				impl->entries[i].value_cmp = duckdb::RequiresValueComparison(t);
				impl->entries[i].finalize_direct = CanonicalEqualsOutput(t);
			}
		} catch (...) {
			/* Unsupported type string -> no stats of any kind for this column. */
		}
		pfree(type_str);
	}
	impl->active = true;
}

InlineColStats::~InlineColStats() = default;

bool
InlineColStats::ColumnEligible(int col) const {
	if (!impl->active || col < 0 || col >= (int)impl->entries.size()) {
		return false;
	}
	return impl->entries[col].eligible;
}

void
InlineColStats::SetupColumn(int col, Oid pg_source_type) {
	if (!impl->active || col < 0 || col >= (int)impl->entries.size()) {
		return;
	}
	auto &e = impl->entries[col];
	if (!e.eligible) {
		return;
	}
	e.src_type = pg_source_type;

	Oid out_func;
	bool out_varlena;
	getTypeOutputInfo(pg_source_type, &out_func, &out_varlena);

	MemoryContext old = MemoryContextSwitchTo(impl->ctx);
	fmgr_info(out_func, &e.out_finfo);
	MemoryContextSwitchTo(old);

	if (e.value_cmp) {
		/* Numeric/temporal/boolean: compare in PG value space via the source type's btree
		 * comparison proc. Its ordering matches DuckDB's value ordering for every eligible
		 * value_cmp type. The FmgrInfo is owned by the (process-lifetime) type cache. */
		TypeCacheEntry *tce = lookup_type_cache(pg_source_type, TYPECACHE_CMP_PROC_FINFO);
		if (!tce || !OidIsValid(tce->cmp_proc_finfo.fn_oid)) {
			/* No usable comparison -> maintain nothing rather than persist an unverifiable bound. */
			e.eligible = false;
			return;
		}
		e.cmp_finfo = &tce->cmp_proc_finfo;
		e.cmp_collation = InvalidOid;
		e.typlen = tce->typlen;
		e.typbyval = tce->typbyval;
	}
	e.ready = true;
}

void
InlineColStats::ObserveDatum(int col, Datum value) {
	if (!impl->active || col < 0 || col >= (int)impl->entries.size()) {
		return;
	}
	auto &e = impl->entries[col];
	if (!e.eligible || !e.ready) {
		return;
	}

	if (e.value_cmp) {
		if (!e.has) {
			/* datumCopy flattens an inline-compressed or short varlena but does NOT detoast an external
			 * pointer -- the copy would then be a dangling TOAST reference. Safe here only because none
			 * of the three call sites can produce one: COPY input functions, deconstruct_array elements
			 * of a freshly built array, and ExecEvalExpr over VALUES constants all yield in-memory
			 * datums. A source that can hand us an external pointer must detoast before calling. */
			MemoryContext old = MemoryContextSwitchTo(impl->ctx);
			e.min_datum = datumCopy(value, e.typbyval, e.typlen);
			e.max_datum = datumCopy(value, e.typbyval, e.typlen);
			MemoryContextSwitchTo(old);
			e.has = true;
			return;
		}
		/* Compare in the caller's (per-tuple) context so comparison temporaries are reclaimed by the
		 * caller's periodic reset; copy a new bound into the accumulator context only when it wins. */
		if (DatumGetInt32(FunctionCall2Coll(e.cmp_finfo, e.cmp_collation, value, e.min_datum)) < 0) {
			MemoryContext old = MemoryContextSwitchTo(impl->ctx);
			Datum copy = datumCopy(value, e.typbyval, e.typlen);
			MemoryContextSwitchTo(old);
			if (!e.typbyval) {
				pfree(DatumGetPointer(e.min_datum));
			}
			e.min_datum = copy;
		}
		if (DatumGetInt32(FunctionCall2Coll(e.cmp_finfo, e.cmp_collation, value, e.max_datum)) > 0) {
			MemoryContext old = MemoryContextSwitchTo(impl->ctx);
			Datum copy = datumCopy(value, e.typbyval, e.typlen);
			MemoryContextSwitchTo(old);
			if (!e.typbyval) {
				pfree(DatumGetPointer(e.max_datum));
			}
			e.max_datum = copy;
		}
		return;
	}

	/* Lexicographic (VARCHAR/UUID): the PG output text equals DuckDB's canonical VARCHAR encoding
	 * for these types, so byte-order string compare here matches DuckLakeColumnStats::MergeStats. */
	char *s = OutputFunctionCall(&e.out_finfo, value);
	std::string cs(s);
	pfree(s);
	if (!e.has) {
		e.min_str = cs;
		e.max_str = cs;
		e.has = true;
		return;
	}
	if (cs < e.min_str) {
		e.min_str = cs;
	}
	if (cs > e.max_str) {
		e.max_str = cs;
	}
}

void
InlineColStats::ObserveNull(int col) {
	if (!impl->active || col < 0 || col >= (int)impl->entries.size()) {
		return;
	}
	impl->entries[col].null_count++;
}

void
InlineColStats::Finalize(std::vector<DirectInsertColumnStat> &out) {
	out.clear();
	if (!impl->active) {
		return;
	}

	/* Temporal output funcs are DateStyle-dependent; force ISO so the text parses back through
	 * DuckDB's cast during canonicalization (and matches what the inline write stored). */
	IsoDateStyleGuard date_style_guard;

	for (auto &e : impl->entries) {
		if (!e.type_known) {
			continue;
		}
		bool have_bounds = e.eligible && e.ready && e.has;

		std::string min_canon;
		std::string max_canon;
		if (have_bounds && e.value_cmp) {
			char *min_txt = OutputFunctionCall(&e.out_finfo, e.min_datum);
			char *max_txt = OutputFunctionCall(&e.out_finfo, e.max_datum);
			if (e.finalize_direct) {
				/* Integer family: PG output text is already byte-identical to DuckDB's canonical
				 * encoding (see CanonicalEqualsOutput), so store it directly -- no Value/cast/ToString. */
				min_canon = min_txt;
				max_canon = max_txt;
				pfree(min_txt);
				pfree(max_txt);
			} else {
				/* Boolean / decimal / temporal: canonicalize the two surviving bounds through the same
				 * Value(text)->cast(type)->ToString path the read side round-trips, so the stored bytes
				 * are exactly what DuckLake would write (e.g. boolean "t"/"f" -> "true"/"false"). */
				bool ok = true;
				try {
					duckdb::Value vmin;
					duckdb::Value vmax;
					std::string err;
					if (!duckdb::Value(std::string(min_txt)).DefaultTryCastAs(e.type, vmin, &err) || vmin.IsNull()) {
						ok = false;
					} else {
						min_canon = vmin.ToString();
					}
					if (ok &&
					    (!duckdb::Value(std::string(max_txt)).DefaultTryCastAs(e.type, vmax, &err) || vmax.IsNull())) {
						ok = false;
					} else if (ok) {
						max_canon = vmax.ToString();
					}
				} catch (...) {
					/* Should be unreachable for inlineable data; skip rather than persist an
					 * unverifiable bound (leaves the existing catalog bound untouched). */
					ok = false;
				}
				pfree(min_txt);
				pfree(max_txt);
				if (!ok) {
					/* Drop the bounds, not the column: an observed NULL is still worth persisting. */
					have_bounds = false;
				}
			}
		} else if (have_bounds) {
			min_canon = e.min_str;
			max_canon = e.max_str;
		}

		if (!have_bounds && e.null_count == 0) {
			continue;
		}

		DirectInsertColumnStat s;
		s.column_id = e.column_id;
		s.column_type = duckdb::DuckLakeTypes::ToString(e.type);
		s.has_min = have_bounds;
		s.has_max = have_bounds;
		if (have_bounds) {
			s.min_value = std::move(min_canon);
			s.max_value = std::move(max_canon);
		}
		s.null_count = e.null_count;
		out.push_back(std::move(s));
	}
}

} // namespace pgducklake
