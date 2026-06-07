#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${ROOT_DIR}/build"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" >/dev/null
cmake --build "${BUILD_DIR}" --target cdrip_test_discogs_title_search >/dev/null
"${BUILD_DIR}/cdrip_test_discogs_title_search"
