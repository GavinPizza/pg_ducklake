#include "pgddb/pg/locale.hpp"

extern "C" {
#include "postgres.h"
}

// Must be outside extern "C" (ICU templates) and after postgres.h (defines USE_ICU), before pg_locale.h.
#ifdef USE_ICU
#include <unicode/ucol.h>
#endif

extern "C" {
#include "utils/pg_locale.h"
}

namespace pgddb::pg {

bool
IsCLocale(Oid collation_id) {
#if PG_VERSION_NUM >= 180000
	return pg_newlocale_from_collation(collation_id)->collate_is_c;
#else
	return lc_ctype_is_c(collation_id);
#endif
}

} // namespace pgddb::pg
