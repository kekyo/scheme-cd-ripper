#!/bin/sh
set -eu

stage_dir=$1
package_name=$2
shift 2
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
mkdir "$tmp_dir/debian"
cat >"$tmp_dir/debian/control" <<EOF
Source: $package_name
Section: misc
Priority: optional
Maintainer: cd-ripper
Standards-Version: 4.6.2

Package: $package_name
Architecture: any
Description: temporary package metadata for dependency calculation
EOF
# Use local shlibs metadata for the private library, then omit self-dependencies.
# Scan the library as well as the CLI to retain all of its runtime dependencies.
printf 'libchafa 0 %s\n' "$package_name" >"$tmp_dir/debian/shlibs.local"
depends_value=$(cd "$tmp_dir" && dpkg-shlibdeps -O \
    -l"$stage_dir/usr/lib/cdrip" -x"$package_name" "$@" | sed -n 's/^shlibs:Depends=//p')
[ -n "$depends_value" ] || {
    echo "dpkg-shlibdeps failed to calculate runtime dependencies for $package_name" >&2
    exit 1
}
printf '%s\n' "$depends_value"
