#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT

CDRIP_PACKAGE_VERSION=0.0.0-test \
CDRIP_PACKAGE_COMMIT=test \
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}"
"${BUILD_DIR}/cdrip_test_activity_observer"
