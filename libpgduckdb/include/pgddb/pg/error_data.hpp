#pragma once

extern "C" {
struct ErrorData;
}

namespace pgddb::pg {
const char *GetErrorDataMessage(ErrorData *error_data);
}
