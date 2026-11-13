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

CDRIP_PACKAGE_VERSION=0.0.0-test CDRIP_PACKAGE_COMMIT=test \
    cmake -S "$ROOT_DIR" -B "$TEST_DIR/build" \
    -DPKG_CONFIG_EXECUTABLE="$TEST_DIR/bin/pkg-config" -DCMAKE_BUILD_TYPE=Release
cmake --build "$TEST_DIR/build" --target cdrip_app cdrip --parallel 4
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
! readelf -d "$TEST_DIR/unused-build/libcdrip.so" | grep -F libchafa

# RUNPATH must allow an interface-compatible replacement without relinking.
mkdir "$TEST_DIR/replacement"
cp -L "$LIB_DIR/libchafa.so.0" "$TEST_DIR/replacement/libchafa.so.0"
LD_LIBRARY_PATH="$TEST_DIR/replacement" ldd "$CLI" | grep -F "libchafa.so.0 => $TEST_DIR/replacement/"
LD_LIBRARY_PATH="$TEST_DIR/replacement" "$CLI" --help >/dev/null
