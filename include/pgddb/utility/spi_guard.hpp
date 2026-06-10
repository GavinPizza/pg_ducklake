#pragma once

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "utils/guc.h"
}

namespace pgddb {

template <bool NewGUCNestLevel_>
class SPIGuard {
public:
	SPIGuard(const SPIGuard &) = delete;
	SPIGuard(SPIGuard &&) = delete;
	SPIGuard &operator=(const SPIGuard &) = delete;
	SPIGuard &operator=(SPIGuard &&) = delete;

	explicit SPIGuard() {
		SPI_connect();
		if (NewGUCNestLevel_)
			save_nestlevel = NewGUCNestLevel();
	}
	~SPIGuard() {
		if (NewGUCNestLevel_)
			AtEOXact_GUC(false, save_nestlevel);
		SPI_finish();
	}

private:
	int save_nestlevel;
};

} // namespace pgddb
