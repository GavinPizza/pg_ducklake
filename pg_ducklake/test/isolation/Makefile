# Makefile for isolation tests

ROOT_DIR = ../../..

include $(ROOT_DIR)/Makefile.pgxs

# pg_duckdb is no longer loaded here (neither preload nor extension):
# CREATE EXTENSION pg_ducklake after pg_duckdb fails on the overlapping
# public read_csv/read_parquet stubs, and no isolation spec uses
# pg_duckdb objects. See isolation.conf for the coexistence status.
check-isolation:
	TEST_DIR=$(CURDIR) $(pg_isolation_regress_installcheck) \
	--encoding=UTF8 \
	--no-locale \
	--temp-instance=./tmp_check \
	--temp-config isolation.conf \
	--load-extension=pg_ducklake \
	$(if $(TEST),$(TEST),--schedule=schedule)

clean-isolation:
	rm -fr $(CURDIR)/tmp_check
	rm -fr $(CURDIR)/output_iso
	rm -f  $(CURDIR)/regression.diffs
	rm -f  $(CURDIR)/regression.out
