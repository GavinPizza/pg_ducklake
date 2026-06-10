#!/usr/bin/env bash
# Local build of the pg_ducklake image. buildx resolves the bake context
# against the CWD, so cd to the repo root first.
set -euo pipefail

cd "$(dirname "$0")/../.."

POSTGRES_VERSION="${POSTGRES_VERSION:-18}"
REPO="${REPO:-pgducklake/pgducklake}"
TARGET="${TARGET:-pg_ducklake_${POSTGRES_VERSION}}"
PUSH="${PUSH:-0}"
PLATFORM="${PLATFORM:-}"

if [[ "${PUSH}" == "1" ]]; then
  OUTPUT_FLAG="--push"
  PLATFORM_SET=()
else
  OUTPUT_FLAG="--load"
  if [[ -z "${PLATFORM}" ]]; then
    case "$(uname -m)" in
      x86_64|amd64) PLATFORM="linux/amd64" ;;
      arm64|aarch64) PLATFORM="linux/arm64" ;;
      *) PLATFORM="linux/amd64" ;;
    esac
  fi
  PLATFORM_SET=(--set "*.platform=${PLATFORM}")
fi

exec docker buildx bake \
  --file pg_ducklake/docker/docker-bake.hcl \
  "${TARGET}" \
  --set "*.args.POSTGRES_VERSION=${POSTGRES_VERSION}" \
  --set "*.tags=${REPO}:${POSTGRES_VERSION}-local" \
  "${PLATFORM_SET[@]}" \
  "${OUTPUT_FLAG}"
