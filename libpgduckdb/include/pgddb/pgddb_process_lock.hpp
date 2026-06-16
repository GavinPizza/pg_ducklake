#pragma once

#include <mutex>

namespace pgddb {

// Serializes PG calls that touch global state (e.g. buffer reads); shared across all threads and replacement scans.
struct GlobalProcessLock {
public:
	static std::recursive_mutex &
	GetLock() {
		static std::recursive_mutex lock;
		return lock;
	}
};

} // namespace pgddb
