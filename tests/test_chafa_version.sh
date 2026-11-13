#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/pkgconfig"
CHAFA_PC=$(pkg-config --variable=pcfiledir chafa)/chafa.pc

for version in 1.14.0 1.14.1 1.14.2; do
    sed "s/^Version:.*/Version: $version/" "$CHAFA_PC" >"$TEST_DIR/pkgconfig/chafa.pc"
    if CDRIP_PACKAGE_VERSION=0.0.0-test CDRIP_PACKAGE_COMMIT=test \
        PKG_CONFIG_PATH="$TEST_DIR/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
        cmake -S "$ROOT_DIR" -B "$TEST_DIR/build-$version" >"$TEST_DIR/configure.log" 2>&1; then
        [ "$version" = 1.14.2 ] || {
            echo "Chafa $version must be rejected: Kitty Base64 output requires 1.14.2 or later" >&2
            exit 1
        }
    else
        [ "$version" != 1.14.2 ] || { cat "$TEST_DIR/configure.log" >&2; exit 1; }
        grep -F 'chafa>=1.14.2' "$TEST_DIR/configure.log" >/dev/null
    fi
done
