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
# Exercise explicit Kitty requests independently of terminal color detection.
for terminal in xterm-256color xterm-kitty; do
    for color in unset '' truecolor; do
        if [ "$color" = unset ]; then
            env -u TMUX -u STY -u COLORTERM TERM="$terminal" script -q -e -c "\"${BUILD_DIR}/cdrip_test_cover_art_selection\" --kitty" /dev/null
        else
            env -u TMUX -u STY TERM="$terminal" COLORTERM="$color" script -q -e -c "\"${BUILD_DIR}/cdrip_test_cover_art_selection\" --kitty" /dev/null
        fi
    done
done
env -u TMUX -u STY TERM=xterm-256color script -q -e -c "\"${BUILD_DIR}/cdrip_test_cover_art_selection\" --diagnostics" /dev/null
