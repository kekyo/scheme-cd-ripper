#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT

assert_contains() {
    local haystack="$1"
    local needle="$2"
    local message="$3"

    if [[ "${haystack}" != *"${needle}"* ]]; then
        echo "assert_contains failed: ${message}" >&2
        echo "  expected to find: ${needle}" >&2
        exit 1
    fi
}

assert_not_contains() {
    local haystack="$1"
    local needle="$2"
    local message="$3"

    if [[ "${haystack}" == *"${needle}"* ]]; then
        echo "assert_not_contains failed: ${message}" >&2
        echo "  unexpected text: ${needle}" >&2
        exit 1
    fi
}

build_and_capture() {
    local version="$1"
    local commit="$2"
    local build_subdir="${BUILD_DIR}/$3"

    CDRIP_PACKAGE_VERSION="${version}" \
    CDRIP_PACKAGE_COMMIT="${commit}" \
    cmake -S "${ROOT_DIR}" -B "${build_subdir}" -DCMAKE_BUILD_TYPE=Release

    cmake --build "${build_subdir}"
    "${build_subdir}/cdrip" 2>&1 || true
}

unknown_output="$(build_and_capture "0.0.0-test" "unknown" "unknown")"
assert_contains "${unknown_output}" "Scheme CD music/sound ripper [0.0.0-test]" "banner should show version"
assert_not_contains "${unknown_output}" "Scheme CD music/sound ripper [0.0.0-test-unknown]" "banner should not expose unknown commit"

commit_output="$(build_and_capture "0.0.0-test" "test" "commit")"
assert_contains "${commit_output}" "Scheme CD music/sound ripper [0.0.0-test-test]" "banner should include known commit"
