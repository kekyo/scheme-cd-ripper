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

assert_fails_contains() {
    local expected_text="$1"
    shift

    local output
    local status
    set +e
    output="$("$@" 2>&1)"
    status=$?
    set -e
    if [[ "${status}" -eq 0 ]]; then
        echo "assert_fails_contains failed: command unexpectedly succeeded" >&2
        echo "  command: $*" >&2
        exit 1
    fi
    assert_contains "${output}" "${expected_text}" "failed command should report expected error"
}

build_and_capture() {
    local version="$1"
    local commit="$2"
    local build_subdir="${BUILD_DIR}/$3"

    CDRIP_PACKAGE_VERSION="${version}" \
    CDRIP_PACKAGE_COMMIT="${commit}" \
    cmake -S "${ROOT_DIR}" -B "${build_subdir}" -DCMAKE_BUILD_TYPE=Release

    cmake --build "${build_subdir}" --target cdrip_app
    local built_cdrip="${build_subdir}/cdrip"
    if [[ ! -x "${built_cdrip}" ]]; then
        echo "built cdrip executable not found: ${built_cdrip}" >&2
        exit 1
    fi
    "${built_cdrip}" --help 2>&1
}

unknown_output="$(build_and_capture "0.0.0-test" "unknown" "unknown")"
assert_contains "${unknown_output}" "Scheme CD music/sound ripper [0.0.0-test]" "banner should show version"
assert_not_contains "${unknown_output}" "Scheme CD music/sound ripper [0.0.0-test-unknown]" "banner should not expose unknown commit"

commit_output="$(build_and_capture "0.0.0-test" "test" "commit")"
assert_contains "${commit_output}" "Scheme CD music/sound ripper [0.0.0-test-test]" "banner should include known commit"
assert_contains "${commit_output}" "--tag / --tags" "help should mention tag overrides"

built_cdrip="${BUILD_DIR}/commit/cdrip"
assert_fails_contains "expected key=value" "${built_cdrip}" --tag artist
assert_fails_contains "tag key must not be empty" "${built_cdrip}" --tag =value
assert_fails_contains "tag value must not be empty" "${built_cdrip}" --tags artist=
