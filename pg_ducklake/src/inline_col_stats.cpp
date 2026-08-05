#include "pgducklake/inline_col_stats.hpp"

#include <cmath>
#include <string>

#include <duckdb.hpp>

#include <common/ducklake_types.hpp>
#include <storage/ducklake_stats.hpp>

extern "C" {
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
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
	bool value_cmp = false; /* RequiresValueComparison(type) */
	bool float_cmp = false; /* FLOAT/DOUBLE: NaN is excluded from min/max and tracked instead */
	bool ready = false;     /* SetupColumn bound a usable PG comparison */
	bool has = false;
	bool saw_nan = false;

	/* PG source-type machinery bound by SetupColumn. */
	Oid src_type = InvalidOid;
	FmgrInfo out_finfo;             /* output function for src_type (Finalize / lexicographic loop) */
	FmgrInfo *cmp_finfo = nullptr;  /* value_cmp: btree comparison proc (owned by the type cache) */
	Oid cmp_collation = InvalidOid; /* value_cmp eligible types are non-collatable */
	int16 typlen = 0;
	bool typbyval = false;

	/* value_cmp path; the copies live in the accumulator context. */
	Datum min_datum = 0;
	Datum max_datum = 0;
	/* lexicographic path; already canonicalized. */
	std::string min_str;
	std::string max_str;
	uint64_t null_count = 0;
};

/* Types whose table-level min/max this accumulator maintains. Membership is an optimization, not
 * the safety property: anything left out is degraded to unknown by the fallback in
 * CreateSnapshotForDirectInsert, so forgetting a type costs pruning, never correctness. Add to it
 * freely, and do not add a read-side liveness argument for the types left out.
 *
 * Gates min/max only -- ObserveNull is deliberately not gated by it.
 *
 * FLOAT/DOUBLE keep NaN out of the bounds and record it in contains_nan, which is what ToStats()
 * consults before trusting a floating-point bound at all. */
bool
StatsEligible(const duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::FLOAT:
	case duckdb::LogicalTypeId::DOUBLE:
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

} // namespace

struct InlineColStats::Impl {
	std::vector<ColStatEntry> entries;
	bool active = false;
	/* Has to outlive the per-tuple/per-batch contexts the callers reset mid-loop. Parented on the
	 * transaction rather than TopMemoryContext because a PG elog(ERROR) longjmps past ~Impl, which
	 * under TopMemoryContext would leak the context once per failed insert. Requires that no caller
	 * outlive the (sub)transaction current at construction. */
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
	/* Bailing leaves every column unobserved, so the fallback degrades the whole table's stats
	 * permanently. Correct, but traceable beats a table that silently loses all pruning. */
	if (ret != SPI_OK_SELECT || (int)SPI_processed < num_cols) {
		elog(DEBUG1,
		     "InlineColStats: no column metadata for table %llu (rc %d, %d rows for %d columns); "
		     "column stats will be degraded",
		     (unsigned long long)table_id, ret, (int)SPI_processed, num_cols);
		return;
	}
	impl->entries.resize(num_cols);
	for (int i = 0; i < num_cols; i++) {
		bool isnull;
		Datum id_d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull) {
			elog(DEBUG1, "InlineColStats: NULL column_id for table %llu; column stats will be degraded",
			     (unsigned long long)table_id);
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
			/* Recorded even for an ineligible type: a null-only stat still goes through the typed
			 * merge. */
			impl->entries[i].type = t;
			impl->entries[i].type_known = true;
			if (StatsEligible(t)) {
				impl->entries[i].eligible = true;
				impl->entries[i].value_cmp = duckdb::RequiresValueComparison(t);
				impl->entries[i].float_cmp =
				    t.id() == duckdb::LogicalTypeId::FLOAT || t.id() == duckdb::LogicalTypeId::DOUBLE;
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

	/* NaN has to be caught before the comparison proc sees it -- float8_cmp_internal orders NaN
	 * above every other value, so one NaN would become the persisted max. That test is
	 * per-representation, so a non-float source type goes to the fallback instead. */
	if (e.float_cmp && pg_source_type != FLOAT4OID && pg_source_type != FLOAT8OID) {
		e.eligible = false;
		return;
	}

	Oid out_func;
	bool out_varlena;
	getTypeOutputInfo(pg_source_type, &out_func, &out_varlena);

	MemoryContext old = MemoryContextSwitchTo(impl->ctx);
	fmgr_info(out_func, &e.out_finfo);
	MemoryContextSwitchTo(old);

	if (e.value_cmp) {
		/* The btree comparison proc's ordering matches DuckDB's value ordering for every eligible
		 * value_cmp type, so comparing in PG value space cannot disagree with the read side. The
		 * FmgrInfo belongs to the process-lifetime type cache, so it is not copied. */
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

	if (e.float_cmp) {
		/* Recording the NaN is what lets the bounds ignore it: the read side builds no
		 * floating-point statistic at all once contains_nan is true. */
		bool is_nan = e.src_type == FLOAT4OID ? std::isnan(DatumGetFloat4(value)) : std::isnan(DatumGetFloat8(value));
		if (is_nan) {
			e.saw_nan = true;
			return;
		}
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
		/* Compared in the caller's per-tuple context so its periodic reset reclaims the comparison
		 * temporaries; only a winning bound is copied into the accumulator context. */
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

	/* VARCHAR/UUID: PG's output text is DuckDB's canonical VARCHAR encoding for these, so a
	 * byte-order compare here agrees with DuckLakeColumnStats::MergeStats. */
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

	/* Output funcs are DateStyle-dependent and the bounds have to cast back through DuckDB. Via the
	 * GUC stack so an error in the loop still restores it, and GUC_ACTION_SAVE because guc.c rejects
	 * SET while libpgduckdb has the backend in parallel mode. */
	int save_nestlevel = NewGUCNestLevel();
	::set_config_option("DateStyle", "ISO, YMD", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

	for (auto &e : impl->entries) {
		if (!e.type_known) {
			continue;
		}
		/* Not conditioned on e.has: a batch of only NULLs or only NaNs still maintained the column,
		 * it just had no bound to contribute. What matters downstream is whether the persisted
		 * bounds still describe every row, not whether they moved. */
		bool bounds_maintained = e.eligible && e.ready;
		bool have_bounds = bounds_maintained && e.has;

		std::string min_canon;
		std::string max_canon;
		if (have_bounds && e.value_cmp) {
			char *min_txt = OutputFunctionCall(&e.out_finfo, e.min_datum);
			char *max_txt = OutputFunctionCall(&e.out_finfo, e.max_datum);
			/* Through the same Value(text)->cast(type)->ToString the read side round-trips, so the
			 * stored bytes are what DuckLake itself would have written ("t" -> "true", 1000 -> 1000.0).
			 *
			 * No shortcut for types whose PG text already looks canonical: this is also the only
			 * validation a bound gets, and the read path casts through the non-Try DefaultCastAs, so an
			 * unparseable literal stored here throws out of GetColumnStats rather than degrading. PG
			 * numeric backs the wider integer types and admits NaN and Infinity, which is how one would
			 * arise. */
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
				/* Unreachable for inlineable data, but an unverifiable bound is worse than none. */
				ok = false;
			}
			pfree(min_txt);
			pfree(max_txt);
			if (!ok) {
				/* The column stays -- an observed NULL is still worth persisting -- but its values
				 * are now undescribed, so the fallback takes the bounds over. */
				have_bounds = false;
				bounds_maintained = false;
			}
		} else if (have_bounds) {
			min_canon = e.min_str;
			max_canon = e.max_str;
		}

		/* Emitted even when it carries nothing. The fallback has to tell "observed, contributed no
		 * bound" apart from "never seen", and only the latter leaves null-ness unmaintained too. */
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
		s.bounds_maintained = bounds_maintained;
		/* Claiming this for a column whose NaN-ness the batch did not determine would let MergeStats
		 * clear the persisted contains_nan. */
		s.has_contains_nan = bounds_maintained && e.float_cmp;
		s.contains_nan = e.saw_nan;
		out.push_back(std::move(s));
	}

	AtEOXact_GUC(false, save_nestlevel);
}

} // namespace pgducklake
