#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

STUB_BIN_DIR="${TMP_DIR}/bin"
BUILD_DIR="${TMP_DIR}/build"
mkdir -p "${STUB_BIN_DIR}"

cat >"${STUB_BIN_DIR}/screw-up" <<'EOF'
#!/bin/sh
set -eu
cat >/dev/null
printf '%s\n' '9.9.9-test'
EOF

cat >"${STUB_BIN_DIR}/cmake" <<'EOF'
#!/bin/sh
set -eu

if [ "$1" = "--build" ]; then
	build_dir=$2
	mkdir -p "$build_dir"
	printf '%s\n' '#!/bin/sh' >"$build_dir/cdrip"
	chmod +x "$build_dir/cdrip"
	: >"$build_dir/libcdrip.so"
	: >"$build_dir/libcdrip.a"
	exit 0
fi

build_dir=''
while [ "$#" -gt 0 ]; do
	case "$1" in
		-B)
			build_dir=$2
			shift 2
			;;
		*)
			shift
			;;
	esac
done

[ -n "$build_dir" ] || exit 1
mkdir -p "$build_dir"
printf '%s\n' "${CDRIP_PACKAGE_VERSION:-}" >"$build_dir/cmake-version.txt"
EOF

cat >"${STUB_BIN_DIR}/dpkg" <<'EOF'
#!/bin/sh
set -eu

if [ "$1" = "--print-architecture" ]; then
	printf '%s\n' 'amd64'
	exit 0
fi

exit 1
EOF

cat >"${STUB_BIN_DIR}/dpkg-shlibdeps" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' 'shlibs:Depends=libc6 (>= 2.38)'
EOF

cat >"${STUB_BIN_DIR}/dpkg-deb" <<'EOF'
#!/bin/sh
set -eu

if [ "$1" = "--build" ]; then
	touch "$2.deb"
	exit 0
fi

exit 1
EOF

chmod +x "${STUB_BIN_DIR}/screw-up" \
    "${STUB_BIN_DIR}/cmake" \
    "${STUB_BIN_DIR}/dpkg" \
    "${STUB_BIN_DIR}/dpkg-shlibdeps" \
    "${STUB_BIN_DIR}/dpkg-deb"

PATH="${STUB_BIN_DIR}:${PATH}" BUILD_DIR="${BUILD_DIR}" "${ROOT_DIR}/build.sh"

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
    local message="$3"

    if ! grep -F "${expected_text}" "${target_path}" >/dev/null 2>&1; then
        echo "assert_contains_text failed: ${message}" >&2
        echo "  path: ${target_path}" >&2
        echo "  expected to find: ${expected_text}" >&2
        exit 1
    fi
}

assert_eq "9.9.9-test" "$(cat "${BUILD_DIR}/cmake-version.txt")" "build.sh should pass screw-up version to cmake"
assert_contains_text "${BUILD_DIR}/deb/libcdrip-dev/DEBIAN/control" "Version: 9.9.9-test" "dev package control should use screw-up version"
assert_contains_text "${BUILD_DIR}/deb/cdrip/DEBIAN/control" "Version: 9.9.9-test" "cli package control should use screw-up version"
