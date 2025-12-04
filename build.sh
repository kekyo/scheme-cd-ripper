#!/bin/bash
set -euo pipefail

# Scheme CD music/sound ripper
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/scheme-cd-ripper

# Simple Debian package builder for libcdrip (shared/static) and cdrip CLI.
# Usage: ARCH=$(dpkg --print-architecture) ./build.sh
# Notes:
# - Requires the project to be buildable with the chosen toolchain.
# - For cross builds set CC/CXX/CMAKE_TOOLCHAIN_FILE as needed and run this script in that environment.
# - Two packages are generated:
#     libcdrip-dev: installs lib and headers (shared + static)
#     cdrip: installs the CLI (statically linked to libcdrip)

usage() {
    echo "Usage: ${BASH_SOURCE[0]} [-d]" >&2
    echo "  -d  Debug build (for Valgrind usage)" >&2
}

BUILD_TYPE="Release"
while getopts ":d" opt; do
    case "${opt}" in
        d)
            BUILD_TYPE="Debug"
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done
shift $((OPTIND - 1))

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(printf '{version}\n' | screw-up format -f 2>/dev/null | head -n 1)"
ARCH="${ARCH:-$(dpkg --print-architecture)}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"

echo "Building for ARCH=${ARCH}, VERSION=${VERSION}, BUILD_TYPE=${BUILD_TYPE}"

rm -rf ${BUILD_DIR}
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}"

PKG_ROOT="${BUILD_DIR}/deb"
rm -rf "${PKG_ROOT}"
mkdir -p "${PKG_ROOT}"

# libcdrip-dev package
DEV_ROOT="${PKG_ROOT}/libcdrip-dev"
mkdir -p "${DEV_ROOT}/DEBIAN" "${DEV_ROOT}/usr/lib" "${DEV_ROOT}/usr/include/cdrip"
cp "${BUILD_DIR}/libcdrip.so"* "${DEV_ROOT}/usr/lib/" 2>/dev/null || true
cp "${BUILD_DIR}/libcdrip.a" "${DEV_ROOT}/usr/lib/" 2>/dev/null || true
cp -r "${ROOT_DIR}/include/cdrip/"* "${DEV_ROOT}/usr/include/cdrip/"
cat > "${DEV_ROOT}/DEBIAN/control" <<EOF
Package: libcdrip-dev
Version: ${VERSION}
Section: libs
Priority: optional
Architecture: ${ARCH}
Maintainer: cd-ripper
Depends: libcdio-paranoia-dev, libcddb2-dev, libflac++-dev, libglib2.0-dev, libsoup-3.0-dev, libjson-glib-dev
Description: C++ CD ripping library (dev package)
EOF

# cdrip package
CLI_ROOT="${PKG_ROOT}/cdrip"
mkdir -p "${CLI_ROOT}/DEBIAN" "${CLI_ROOT}/usr/bin"
cp "${BUILD_DIR}/cdrip" "${CLI_ROOT}/usr/bin/"
SHLIBDEPS_TMP="$(mktemp -d)"
trap 'rm -rf "${SHLIBDEPS_TMP}"' EXIT
mkdir -p "${SHLIBDEPS_TMP}/debian"
cat > "${SHLIBDEPS_TMP}/debian/control" <<EOF
Source: cdrip
Section: sound
Priority: optional
Maintainer: cd-ripper
Standards-Version: 4.6.2

Package: cdrip
Architecture: ${ARCH}
Description: CLI CD ripper using libcdrip
EOF
CLI_DEPS="$(cd "${SHLIBDEPS_TMP}" && dpkg-shlibdeps -O "${CLI_ROOT}/usr/bin/cdrip" | sed -n 's/^shlibs:Depends=//p')"
if [[ -z "${CLI_DEPS}" ]]; then
    echo "dpkg-shlibdeps failed to calculate runtime dependencies" >&2
    exit 1
else
    echo "Calculated runtime deps: ${CLI_DEPS}"
fi
cat > "${CLI_ROOT}/DEBIAN/control" <<EOF
Package: cdrip
Version: ${VERSION}
Section: sound
Priority: optional
Architecture: ${ARCH}
Maintainer: cd-ripper
Depends: ${CLI_DEPS}
Description: CLI CD ripper using libcdrip
EOF

dpkg-deb --build "${DEV_ROOT}"
dpkg-deb --build "${CLI_ROOT}"

echo "Packages created under ${PKG_ROOT}:"
ls -1 "${PKG_ROOT}"/*.deb
mkdir -p "${PKG_ROOT}"
