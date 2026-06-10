#include "pgddb/pg/error_data.hpp"

extern "C" {
#include "postgres.h"
}

namespace pgddb::pg {
const char *
GetErrorDataMessage(ErrorData *error_data) {
	return error_data->message;
}
} // namespace pgddb::pg
