#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT

CDRIP_PACKAGE_VERSION=0.0.0-test \
CDRIP_PACKAGE_COMMIT=test \
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}"
"${BUILD_DIR}/cdrip_test_cover_art_selection"
# Exercise image payloads and cursor handling with an actual PTY.
env -u TMUX -u STY TERM=xterm-kitty script -q -e -c "\"${BUILD_DIR}/cdrip_test_cover_art_selection\" --kitty" /dev/null
