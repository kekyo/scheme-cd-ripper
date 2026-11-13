#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/bin"
export CDRIP_TEST_PKG_CONFIG
CDRIP_TEST_PKG_CONFIG=$(command -v pkg-config)
cat >"$TEST_DIR/bin/pkg-config" <<'EOF'
#!/bin/sh
for arg do
    case "$arg" in
        *chafa*) echo 'System Chafa must not be required' >&2; exit 1 ;;
    esac
done
exec "$CDRIP_TEST_PKG_CONFIG" "$@"
EOF
chmod +x "$TEST_DIR/bin/pkg-config"

# A source distribution must build offline, including the submodule contents.
"$ROOT_DIR/scripts/create_source_archive.sh" "$TEST_DIR/source.tar.xz"
mkdir "$TEST_DIR/source"
tar -xJf "$TEST_DIR/source.tar.xz" -C "$TEST_DIR/source"
test ! -e "$TEST_DIR/source/.git"
test ! -e "$TEST_DIR/source/deps/chafa/.git"
cmp "$ROOT_DIR/deps/chafa/chafa/internal/chafa-base64.c" "$TEST_DIR/source/deps/chafa/chafa/internal/chafa-base64.c"
"$TEST_DIR/source/scripts/create_source_archive.sh" "$TEST_DIR/repacked.tar.xz"

CDRIP_PACKAGE_VERSION=0.0.0-test CDRIP_PACKAGE_COMMIT=test \
    cmake -S "$TEST_DIR/source" -B "$TEST_DIR/build" \
    -DPKG_CONFIG_EXECUTABLE="$TEST_DIR/bin/pkg-config" -DCMAKE_BUILD_TYPE=Release
if ! cmake --build "$TEST_DIR/build" --target cdrip_app cdrip --parallel 4 >"$TEST_DIR/build.log" 2>&1; then
    cat "$TEST_DIR/build.log" >&2
    exit 1
fi
if grep -i 'warning:' "$TEST_DIR/build.log"; then
    echo 'The bundled build must not introduce compiler warnings' >&2
    exit 1
fi
DESTDIR="$TEST_DIR/stage" cmake --install "$TEST_DIR/build" --prefix /usr --component cli

# Installed executables must keep working after the build tree is removed.
mv "$TEST_DIR/build" "$TEST_DIR/unused-build"
mv "$TEST_DIR/stage" "$TEST_DIR/relocated"
CLI="$TEST_DIR/relocated/usr/bin/cdrip"
LIB_DIR="$TEST_DIR/relocated/usr/lib/cdrip"
test -f "$LIB_DIR/libchafa.so.0"
readelf -d "$CLI" | grep -F '(RUNPATH)' | grep -F '$ORIGIN/../lib/cdrip'
! readelf -d "$CLI" | grep -F "$TEST_DIR"
env -u LD_LIBRARY_PATH ldd "$CLI" | grep -F "libchafa.so.0 => $TEST_DIR/relocated/usr/bin/../lib/cdrip/"
env -u LD_LIBRARY_PATH "$CLI" --help >/dev/null
env -u LD_LIBRARY_PATH "$CLI" --help | grep -F 'Chafa: Copyright (C) 2018-2023 Hans Petter Jansson'
env -u LD_LIBRARY_PATH "$CLI" --help | grep -F 'LGPL-3.0-or-later'
! readelf -d "$TEST_DIR/unused-build/libcdrip.so" | grep -F libchafa
DOC_DIR="$TEST_DIR/relocated/usr/share/doc/cdrip"
cmp "$ROOT_DIR/deps/chafa/COPYING" "$DOC_DIR/chafa/COPYING"
cmp "$ROOT_DIR/deps/chafa/COPYING.LESSER" "$DOC_DIR/chafa/COPYING.LESSER"
cmp "$ROOT_DIR/deps/chafa/chafa/internal/smolscale/COPYING" "$DOC_DIR/chafa/smolscale/COPYING"
test -f "$DOC_DIR/THIRD_PARTY.md"

mkdir "$TEST_DIR/relocated/DEBIAN"
depends=$("$ROOT_DIR/scripts/package_shlibdeps.sh" "$TEST_DIR/relocated" cdrip "$CLI" "$LIB_DIR/libchafa.so.0")
printf '%s\n' "$depends" | grep -F libc6
printf '%s\n' "$depends" | grep -E 'libglib2.0-0(t64)?'
! printf '%s\n' "$depends" | grep -E 'libchafa|(^|, )cdrip( |,|$)'

# RUNPATH must allow an interface-compatible replacement without relinking.
mkdir "$TEST_DIR/replacement"
cp -L "$LIB_DIR/libchafa.so.0" "$TEST_DIR/replacement/libchafa.so.0"
LD_LIBRARY_PATH="$TEST_DIR/replacement" ldd "$CLI" | grep -F "libchafa.so.0 => $TEST_DIR/replacement/"
LD_LIBRARY_PATH="$TEST_DIR/replacement" "$CLI" --help >/dev/null
