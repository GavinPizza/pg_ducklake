# pg_ducklake Docker Image

This image extends the official Postgres image with the `pg_ducklake`
extension pre-installed and initialized in the default database.

## Local build

```bash
pg_ducklake/docker/docker-build.sh
```

Environment variables:

- `POSTGRES_VERSION` (default: `18`)
- `REPO` (default: `pgducklake/pgducklake`)
- `PUSH=1` to push instead of loading locally

The build context is the repo root (the image compiles the libpgddb kernel
and the duckdb submodule); see `docker-bake.hcl` and `Dockerfile`.

## Usage

Use the image like the official Postgres image:

```bash
docker run -d -e POSTGRES_PASSWORD=duckdb pgducklake/pgducklake:18-main
```
