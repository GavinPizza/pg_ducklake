# pg_ducklake image: the official postgres image + the pg_ducklake extension.
# Build context is the REPO ROOT (the build needs the libpgddb kernel and the
# duckdb submodule); ignore rules live in Dockerfile.dockerignore.
FROM postgres_base AS base

###
### BUILDER
###
FROM base AS builder
ARG POSTGRES_VERSION

RUN apt-get update -qq && \
    apt-get install -y \
    postgresql-server-dev-${POSTGRES_VERSION} \
    build-essential libreadline-dev zlib1g-dev flex bison libxml2-dev libxslt-dev \
    libssl-dev libxml2-utils xsltproc pkg-config libc++-dev libc++abi-dev libglib2.0-dev \
    libtinfo6 cmake libstdc++-12-dev liblz4-dev ccache ninja-build git libcurl4-openssl-dev \
    curl zip unzip && \
    rm -rf /var/lib/apt/lists/*

# vcpkg provides CRoaring for DuckLake >= v1.5 deletion vectors (declared in
# the vendored ducklake's vcpkg.json), pinned to the same commit as CI.
# VCPKG_FORCE_SYSTEM_BINARIES is required on linux/arm64 (harmless on amd64).
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git clone https://github.com/microsoft/vcpkg.git /vcpkg && \
    git -C /vcpkg checkout 84bab45d415d22042bd0b9081aea57f362da3f35 && \
    /vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_TOOLCHAIN_PATH=/vcpkg/scripts/buildsystems/vcpkg.cmake
# vcpkg triplet for the target platform, read back in the make invocations.
ARG TARGETARCH
RUN { [ "$TARGETARCH" = "arm64" ] && echo arm64-linux || echo x64-linux; } > /vcpkg/triplet

WORKDIR /build

ENV PATH=/usr/lib/ccache:$PATH
ENV CCACHE_DIR=/ccache

# permissions so we can run as `postgres` (uid=999,gid=999); vcpkg writes
# downloads/ and buildtrees/ under its root during the duckdb configure.
RUN mkdir /out
RUN chown -R postgres:postgres . /usr/lib/postgresql /usr/share/postgresql /out /vcpkg
USER postgres

### duckdb bundle layer -- heavy; only invalidated by the build wiring, the
### duckdb submodule, or the vendored ducklake inputs.
COPY --chown=postgres:postgres Makefile Makefile.pgxs ./
# Satisfy the duckdb submodule init-stamp so make skips `git submodule
# update`; the duckdb source itself is COPY'd in directly.
COPY --chown=postgres:postgres .git/modules/duckdb/HEAD .git/modules/duckdb/HEAD
COPY --chown=postgres:postgres duckdb duckdb
# The ducklake submodule ships WITH its gitdir (whitelisted in
# Dockerfile.dockerignore) at the same relative layout, so the Makefile's
# patch-stamp rule (git checkout/clean/apply) works inside the build.
COPY --chown=postgres:postgres .git/modules/pg_ducklake/third_party/ducklake .git/modules/pg_ducklake/third_party/ducklake
COPY --chown=postgres:postgres pg_ducklake/Makefile pg_ducklake/pg_ducklake_extensions.cmake pg_ducklake/
COPY --chown=postgres:postgres pg_ducklake/third_party pg_ducklake/third_party

RUN --mount=type=cache,target=/ccache/,uid=999,gid=999 \
    VCPKG_TARGET_TRIPLET="$(cat /vcpkg/triplet)" make -j$(nproc) -C pg_ducklake duckdb

### extension layer
COPY --chown=postgres:postgres libpgduckdb libpgduckdb
COPY --chown=postgres:postgres pg_ducklake/pg_ducklake.control pg_ducklake/
COPY --chown=postgres:postgres pg_ducklake/sql pg_ducklake/sql
COPY --chown=postgres:postgres pg_ducklake/src pg_ducklake/src
COPY --chown=postgres:postgres pg_ducklake/include pg_ducklake/include

RUN --mount=type=cache,target=/ccache/,uid=999,gid=999 \
    VCPKG_TARGET_TRIPLET="$(cat /vcpkg/triplet)" make -j$(nproc) -C pg_ducklake
# install into location specified by pg_config for tests
RUN VCPKG_TARGET_TRIPLET="$(cat /vcpkg/triplet)" make -C pg_ducklake install
# install into /out for packaging
RUN VCPKG_TARGET_TRIPLET="$(cat /vcpkg/triplet)" DESTDIR=/out make -C pg_ducklake install

###
### CHECKER
###
FROM builder AS checker

USER postgres
COPY --chown=postgres:postgres pg_ducklake/test pg_ducklake/test
# Regression schedule only: the isolation suite loads pg_duckdb, which this
# image intentionally does not ship.
RUN make -C pg_ducklake check-regression

###
### SLIM
###
FROM base AS slim

RUN apt-get update -qq && \
    apt-get install -y ca-certificates libcurl4 && \
    rm -rf /var/lib/apt/lists/*

COPY --chown=postgres:postgres pg_ducklake/docker/init.d/ /docker-entrypoint-initdb.d/

COPY --from=builder /out /
USER postgres

###
### OUTPUT (full image with debugging tools)
###
FROM slim AS output
USER root

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
    procps vim-tiny less curl net-tools iputils-ping htop lsof strace gdb && \
    ln -sf /usr/bin/vim.tiny /usr/bin/vim && \
    rm -rf /var/lib/apt/lists/*

USER postgres
