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
    assert_eq "15" "$(count_deb_builds)" "full matrix count"

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

test_prereq_image_name() {
    assert_eq \
        "localhost/scheme-cd-ripper-pack-deb-debian-bookworm-x86_64:latest" \
        "$(prereq_image_for_target debian bookworm x86_64)" \
        "prereq image should include distro, release, and canonical arch"
    assert_eq \
        "localhost/scheme-cd-ripper-pack-deb-ubuntu-24.04-arm64:latest" \
        "$(prereq_image_for_target ubuntu 24.04 arm64)" \
        "prereq image should preserve Ubuntu release numbers"
}

test_assert_prereq_image_requires_existing_image() {
    local tmp_dir
    local stub_engine
    tmp_dir="$(mktemp -d)"
    stub_engine="${tmp_dir}/container-engine"

    cat >"${stub_engine}" <<'EOF'
#!/bin/bash
set -euo pipefail
if [[ "${1:-}" == "image" && "${2:-}" == "exists" ]]; then
    exit 1
fi
exit 2
EOF
    chmod +x "${stub_engine}"

    if ( CONTAINER_ENGINE_BIN="${stub_engine}"; assert_prereq_image "localhost/missing:latest" ) >"${tmp_dir}/stdout.txt" 2>"${tmp_dir}/stderr.txt"; then
        echo "assert_prereq_image unexpectedly succeeded" >&2
        exit 1
    fi

    assert_contains_text "${tmp_dir}/stderr.txt" "Missing prerequisite image: localhost/missing:latest. Run ./prereq.sh first."

    rm -rf "${tmp_dir}"
}

test_build_deb_packages_uses_prereq_image() {
    (
        local tmp_dir
        local project_dir
        local bin_dir
        local stub_engine
        local records_path
        local dpkg_records_path

        tmp_dir="$(mktemp -d)"
        project_dir="${tmp_dir}/project"
        bin_dir="${tmp_dir}/bin"
        stub_engine="${bin_dir}/container-engine"
        records_path="${tmp_dir}/container-records.txt"
        dpkg_records_path="${tmp_dir}/dpkg-records.txt"

        mkdir -p "${project_dir}" "${bin_dir}"

        cat >"${stub_engine}" <<'EOF'
#!/bin/bash
set -euo pipefail

if [[ "${1:-}" == "image" && "${2:-}" == "exists" ]]; then
    printf 'exists %s\n' "${3:-}" >>"${CDRIP_CONTAINER_STUB_RECORDS}"
    exit 0
fi

if [[ "${1:-}" != "run" ]]; then
    printf 'Unexpected container command: %s\n' "$*" >&2
    exit 2
fi

workspace=''
script_index=-1
args=("$@")
for ((index = 0; index < ${#args[@]}; index += 1)); do
    if [[ "${args[index]}" == "-v" ]]; then
        value="${args[index + 1]:-}"
        if [[ "${value}" =~ ^(.*):/workspace(:.*)?$ ]]; then
            workspace="${BASH_REMATCH[1]}"
        fi
        index=$((index + 1))
        continue
    fi

    if [[ "${args[index]}" == ./scripts/* ]]; then
        script_index="${index}"
    fi
done

if [[ "${workspace}" == "" || "${script_index}" -le 0 ]]; then
    printf 'Container run did not include the expected workspace and script.\n' >&2
    exit 2
fi

image="${args[script_index - 1]}"
printf 'run %s\n' "${image}" >>"${CDRIP_CONTAINER_STUB_RECORDS}"

if [[ "${image}" != "localhost/scheme-cd-ripper-pack-deb-debian-bookworm-x86_64:latest" ]]; then
    printf 'Unexpected image: %s\n' "${image}" >&2
    exit 3
fi

stage_root="${workspace}/artifacts/.tmp/test-run/deb/debian/bookworm/x86_64/work/stage"
mkdir -p "${stage_root}/libcdrip-dev" "${stage_root}/cdrip"
EOF

        cat >"${bin_dir}/dpkg-deb" <<'EOF'
#!/bin/bash
set -euo pipefail
output_path="${@: -1}"
mkdir -p "$(dirname "${output_path}")"
printf 'stub-deb\n' >"${output_path}"
printf 'dpkg-deb %s\n' "$*" >>"${CDRIP_DPKG_STUB_RECORDS}"
EOF
        chmod +x "${stub_engine}" "${bin_dir}/dpkg-deb"

        PROJECT_ROOT="${project_dir}"
        ARTIFACT_ROOT="${project_dir}/artifacts"
        DEB_ARTIFACT_ROOT="${ARTIFACT_ROOT}/deb"
        TMP_ROOT="${ARTIFACT_ROOT}/.tmp/test-run"
        RUN_ID="test-run"
        VERSION="1.2.3-test"
        CONTAINER_ENGINE_BIN="${stub_engine}"
        MAKE_JOBS=1
        BUILD_TYPE="Release"
        PATH="${bin_dir}:${PATH}"
        export CDRIP_CONTAINER_STUB_RECORDS="${records_path}"
        export CDRIP_DPKG_STUB_RECORDS="${dpkg_records_path}"

        build_deb_packages debian bookworm x86_64 linux/amd64

        assert_contains_text "${records_path}" "exists localhost/scheme-cd-ripper-pack-deb-debian-bookworm-x86_64:latest"
        assert_contains_text "${records_path}" "run localhost/scheme-cd-ripper-pack-deb-debian-bookworm-x86_64:latest"
        assert_contains_text "${dpkg_records_path}" "${DEB_ARTIFACT_ROOT}/libcdrip-dev-1.2.3-test-debian-bookworm-amd64.deb"
        assert_contains_text "${dpkg_records_path}" "${DEB_ARTIFACT_ROOT}/cdrip-1.2.3-test-debian-bookworm-amd64.deb"

        rm -rf "${tmp_dir}"
    )
}

test_prereq_script_builds_filtered_image() {
    local tmp_dir
    local project_dir
    local bin_dir
    local stub_engine
    local records_path

    tmp_dir="$(mktemp -d)"
    project_dir="${tmp_dir}/project"
    bin_dir="${tmp_dir}/bin"
    stub_engine="${bin_dir}/container-engine"
    records_path="${tmp_dir}/prereq-records.txt"

    mkdir -p "${project_dir}" "${bin_dir}"
    ln -s "${ROOT_DIR}/build_package.sh" "${project_dir}/build_package.sh"

    cat >"${stub_engine}" <<'EOF'
#!/bin/bash
set -euo pipefail

if [[ "${1:-}" == "build" ]]; then
    containerfile=''
    args=("$@")
    for ((index = 0; index < ${#args[@]}; index += 1)); do
        if [[ "${args[index]}" == "-f" ]]; then
            containerfile="${args[index + 1]:-}"
            break
        fi
    done

    printf '%s\n' "$*" >"${CDRIP_PREREQ_STUB_RECORDS}"
    grep -F "libcdio-paranoia-dev" "${containerfile}" >>"${CDRIP_PREREQ_STUB_RECORDS}"
    grep -F "libsoup-3.0-dev" "${containerfile}" >>"${CDRIP_PREREQ_STUB_RECORDS}"
    exit 0
fi

printf 'Unexpected container command: %s\n' "$*" >&2
exit 2
EOF
    chmod +x "${stub_engine}"

    CDRIP_PREREQ_STUB_RECORDS="${records_path}" \
    CDRIP_PREREQ_PROJECT_ROOT="${project_dir}" \
    CONTAINER_ENGINE="${stub_engine}" \
        "${ROOT_DIR}/prereq.sh" --distro debian --release bookworm --arch amd64 --jobs 1 --force >"${tmp_dir}/stdout.txt"

    assert_contains_text "${records_path}" "build --no-cache --platform linux/amd64"
    assert_contains_text "${records_path}" "BASE_IMAGE=docker.io/amd64/debian:bookworm"
    assert_contains_text "${records_path}" "localhost/scheme-cd-ripper-pack-deb-debian-bookworm-x86_64:latest"
    assert_contains_text "${records_path}" "libcdio-paranoia-dev"
    assert_contains_text "${records_path}" "libsoup-3.0-dev"
    assert_contains_text "${tmp_dir}/stdout.txt" "Prerequisite images are ready."

    rm -rf "${tmp_dir}"
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
    assert_contains_text "${tmp_dir}/stderr.txt" "Run ./prereq.sh --force to rebuild prerequisite images"

    rm -rf "${tmp_dir}"
}

test_canonical_filters
test_count_deb_builds
test_artifact_path
test_prereq_image_name
test_assert_prereq_image_requires_existing_image
test_build_deb_packages_uses_prereq_image
test_prereq_script_builds_filtered_image
test_build_package_all_wrapper
