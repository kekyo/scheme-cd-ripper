#!/bin/bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT
CDRIP_PACKAGE_VERSION=0.0.0-test CDRIP_PACKAGE_COMMIT=test \
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target cdrip_test_cli_config
"${BUILD_DIR}/cdrip_test_cli_config"
for option in --unknown-option; do
    if "${BUILD_DIR}/cdrip_test_cli_config" "$option"; then
        echo "Unexpectedly accepted $option" >&2
        exit 1
    fi
done
for option in -d -f -m -c -w -i -ft -dc -v -ro; do
    if "${BUILD_DIR}/cdrip_test_cli_config" "$option"; then
        echo "Unexpectedly accepted missing value for $option" >&2
        exit 1
    fi
done
"${BUILD_DIR}/cdrip_test_cli_config" -c auto -v kitty -nv -na -ro -6
