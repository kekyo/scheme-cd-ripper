#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export BUILD_PACKAGE_PROJECT_ROOT="${ROOT_DIR}"
BUILD_PACKAGE_SOURCE_ONLY=1 source "${ROOT_DIR}/build_package.sh"
unset BUILD_PACKAGE_PROJECT_ROOT

assert_eq() {
    local expected="$1"
    local actual="$2"
    local message="$3"

    if [[ "${expected}" != "${actual}" ]]; then
        echo "assert_eq failed: ${message}" >&2
        echo "  expected: ${expected}" >&2
        echo "  actual:   ${actual}" >&2
        exit 1
    fi
}

assert_contains_text() {
    local target_path="$1"
    local expected_text="$2"

    if ! grep -F "${expected_text}" "${target_path}" >/dev/null 2>&1; then
        echo "assert_contains_text failed: ${target_path} does not contain ${expected_text}" >&2
        exit 1
    fi
}

test_canonical_filters() {
    assert_eq "x86_64" "$(canonical_arch amd64)" "amd64 should map to x86_64"
    assert_eq "i686" "$(canonical_arch i386)" "i386 should map to i686"
    assert_eq "armv7l" "$(canonical_arch armhf)" "armhf should map to armv7l"
    assert_eq "24.04" "$(canonical_release noble)" "noble should map to 24.04"
    assert_eq "22.04" "$(canonical_release jammy)" "jammy should map to 22.04"
    assert_eq "debian,ubuntu" "$(normalize_filter_list distro "debian,ubuntu")" "distro filters should stay canonical"
    assert_eq "x86_64,armv7l" "$(normalize_filter_list arch "amd64,armhf")" "arch filters should normalize aliases"
}

test_count_deb_builds() {
    DISTRO_FILTER=''
    RELEASE_FILTER=''
    ARCH_FILTER=''
    assert_eq "13" "$(count_deb_builds)" "full matrix count"

    DISTRO_FILTER="$(normalize_filter_list distro ubuntu)"
    RELEASE_FILTER="$(normalize_filter_list release 24.04)"
    ARCH_FILTER="$(normalize_filter_list arch arm64)"
    assert_eq "1" "$(count_deb_builds)" "single target filter should resolve to one build"

    DISTRO_FILTER="$(normalize_filter_list distro debian)"
    RELEASE_FILTER="$(normalize_filter_list release trixie)"
    ARCH_FILTER="$(normalize_filter_list arch riscv64)"
    assert_eq "1" "$(count_deb_builds)" "riscv64 filter should resolve to one build"
}

test_artifact_path() {
    VERSION="1.2.3"
    assert_eq \
        "${ROOT_DIR}/artifacts/deb/cdrip-1.2.3-ubuntu-24.04-amd64.deb" \
        "$(deb_artifact_path cdrip ubuntu 24.04 x86_64)" \
        "artifact path should use Debian architecture aliases"
}

test_build_package_all_wrapper() {
    local tmp_dir
    local stub_script
    tmp_dir="$(mktemp -d)"
    stub_script="${tmp_dir}/build_package_stub.sh"

    cat >"${stub_script}" <<'EOF'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$@" >"${BUILD_PACKAGE_ALL_CAPTURE}"
EOF
    chmod +x "${stub_script}"

    BUILD_PACKAGE_SCRIPT="${stub_script}" \
    BUILD_PACKAGE_ALL_CAPTURE="${tmp_dir}/args.txt" \
    "${ROOT_DIR}/build_package_all.sh" --jobs 3 --arch amd64 --refresh-base >"${tmp_dir}/stdout.txt" 2>"${tmp_dir}/stderr.txt"

    mapfile -t captured_args <"${tmp_dir}/args.txt"
    assert_eq "--target" "${captured_args[0]}" "wrapper should inject --target"
    assert_eq "all" "${captured_args[1]}" "wrapper should force all target"
    assert_eq "--jobs" "${captured_args[2]}" "wrapper should forward --jobs"
    assert_eq "3" "${captured_args[3]}" "wrapper should forward jobs value"
    assert_eq "--arch" "${captured_args[4]}" "wrapper should forward --arch"
    assert_eq "amd64" "${captured_args[5]}" "wrapper should preserve forwarded arguments"
    assert_contains_text "${tmp_dir}/stderr.txt" "ignored in the podman-based builder"

    rm -rf "${tmp_dir}"
}

test_canonical_filters
test_count_deb_builds
test_artifact_path
test_build_package_all_wrapper
