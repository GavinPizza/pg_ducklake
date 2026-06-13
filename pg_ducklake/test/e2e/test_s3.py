# S3-specific e2e tests: pg_ducklake writing table data to a MinIO bucket via
# ducklake.default_table_path plus a DuckDB S3 secret. Skips without S3 infra
# (unless E2E_REQUIRE_S3=1); storage-agnostic behavior lives in test_crud.py.

import asyncpg
import pytest

from conftest import Lake


@pytest.fixture
def s3lake(cluster, db, s3):
    return Lake(cluster, db, s3)


@pytest.fixture
async def s3conn(s3lake):
    c = await s3lake.connect()
    # Disable inlining (a persistent catalog option) so writes land in the
    # bucket immediately and object assertions are deterministic; the inlined
    # s3 path is still covered by test_crud.py::test_copy_from_stdin[s3].
    await c.execute("CALL ducklake.set_option('data_inlining_row_limit', 0)")
    try:
        yield c
    finally:
        await c.close()


async def test_parquet_objects_land_in_bucket(s3lake, s3conn, s3):
    # data inlining is disabled by default, so INSERT writes parquet
    # straight to the bucket
    await s3conn.execute("CREATE TABLE t (id int, name text) USING ducklake")
    await s3conn.execute("INSERT INTO t VALUES (1, 'alice'), (2, 'bob')")

    objects = s3.object_names()
    assert objects, "no objects appeared in the bucket after INSERT"
    assert any(name.endswith(".parquet") for name in objects)
    # rows read back from s3, not from any local cache of the insert
    await s3conn.execute("CALL ducklake.recycle_ddb()")
    await s3lake.configure_s3(s3conn)  # recycle dropped the secret
    rows = await s3conn.fetch("SELECT id, name FROM t ORDER BY id")
    assert [tuple(r) for r in rows] == [(1, "alice"), (2, "bob")]


async def test_metadata_reports_s3_paths(s3lake, s3conn):
    await s3conn.execute("CREATE TABLE t (id int) USING ducklake")
    await s3conn.execute("INSERT INTO t VALUES (1)")

    assert (
        await s3conn.fetchval(
            "SELECT count(*) FROM ducklake.list_files('public', 't')"
        )
        > 0
    )
    # the duckdb_query escape hatch can inspect file paths; its duckdb_row
    # result shape needs the simple protocol, so go through psql
    out = s3lake.psql(
        f"""SELECT * FROM ducklake.duckdb_query($q$
              SELECT bool_and(starts_with(data_file, '{s3lake.table_path}'))
              FROM ducklake_list_files('pgducklake', 't', schema => 'public')
            $q$)"""
    )
    assert out.strip().splitlines()[-1] == "t"


async def test_fresh_backend_needs_secret(s3lake, s3conn):
    await s3conn.execute("CREATE TABLE t (id int) USING ducklake")
    await s3conn.execute("INSERT INTO t VALUES (1), (2)")

    # A brand-new backend has a brand-new DuckDB instance with no S3
    # secret: reading s3-backed data must fail rather than silently
    # succeed off some cache (AWS_* env is scrubbed by the harness).
    bare = await s3lake.connect(configure=False)
    try:
        # match the bucket name so an unrelated error cannot satisfy the
        # test: the failure must come from the s3 data-file access
        with pytest.raises(asyncpg.PostgresError, match=s3lake.s3.bucket):
            await bare.fetch("SELECT * FROM t")
        await s3lake.configure_s3(bare)
        assert await bare.fetchval("SELECT count(*) FROM t") == 2
    finally:
        await bare.close()


async def test_recycle_ddb_drops_secret(s3lake, s3conn):
    await s3conn.execute("CREATE TABLE t (id int) USING ducklake")
    await s3conn.execute("INSERT INTO t VALUES (1)")
    assert await s3conn.fetchval("SELECT count(*) FROM t") == 1

    # recycle_ddb tears down the per-backend DuckDB instance; plain
    # (non-persistent) secrets die with it
    await s3conn.execute("CALL ducklake.recycle_ddb()")
    with pytest.raises(asyncpg.PostgresError, match=s3lake.s3.bucket):
        await s3conn.fetch("SELECT * FROM t")

    await s3lake.configure_s3(s3conn)
    assert await s3conn.fetchval("SELECT count(*) FROM t") == 1


async def test_update_delete_against_s3(s3conn, s3):
    await s3conn.execute("CREATE TABLE t (id int, val int) USING ducklake")
    await s3conn.execute(
        "INSERT INTO t SELECT g, g * 10 FROM generate_series(1, 100) AS g(g)"
    )
    objects_before = set(s3.object_names())

    tag = await s3conn.execute("UPDATE t SET val = -val WHERE id <= 10")
    assert tag == "UPDATE 10"
    tag = await s3conn.execute("DELETE FROM t WHERE id > 90")
    assert tag == "DELETE 10"

    # update/delete on DuckLake are copy-on-write: new data/delete files
    # must have been written to the bucket, never in-place mutation
    objects_after = set(s3.object_names())
    assert objects_before < objects_after

    assert await s3conn.fetchval("SELECT count(*) FROM t") == 90
    assert await s3conn.fetchval("SELECT min(val) FROM t") == -100
    assert await s3conn.fetchval("SELECT max(id) FROM t") == 90


async def test_time_travel_on_s3(s3conn):
    await s3conn.execute("CREATE TABLE t (id int) USING ducklake")
    v0 = await s3conn.fetchval(
        "SELECT max(snapshot_id) FROM ducklake.ducklake_snapshot"
    )
    await s3conn.execute("INSERT INTO t VALUES (1)")
    await s3conn.execute("INSERT INTO t VALUES (2)")
    assert (
        await s3conn.fetchval(
            f"SELECT count(*) FROM ducklake.time_travel('t', {v0 + 1})"
        )
        == 1
    )


async def test_duckdb_client_reads_s3_lake(s3lake, s3conn):
    await s3conn.execute("CREATE TABLE t (id int, name text) USING ducklake")
    await s3conn.execute("INSERT INTO t VALUES (1, 'alice'), (2, 'bob')")

    # an external duckdb process needs its own S3 secret to read the data
    # files; the metadata comes over the postgres protocol
    ddb = s3lake.duckdb(read_only=True)
    try:
        rows = ddb.execute(
            "SELECT id, name FROM lake.public.t ORDER BY id"
        ).fetchall()
        assert rows == [(1, "alice"), (2, "bob")]
    finally:
        ddb.close()
