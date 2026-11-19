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
"${BUILD_DIR}/cdrip_test_cli_config" --measure-offset
"${BUILD_DIR}/cdrip_test_cli_config" --measure-offset 5 -d /dev/sr0
for value in 0 -1 1.5 abc 2147483648; do
    if "${BUILD_DIR}/cdrip_test_cli_config" --measure-offset "$value"; then
        echo "Unexpectedly accepted measurement count $value" >&2
        exit 1
    fi
done
if "${BUILD_DIR}/cdrip_test_cli_config" --measure-offset --no-accurate ||
   "${BUILD_DIR}/cdrip_test_cli_config" --measure-offset --update . ||
   "${BUILD_DIR}/cdrip_test_cli_config" --measure-offset --read-offset 0; then
    echo "Unexpectedly accepted conflicting measurement options" >&2
    exit 1
fi
