#!/bin/sh

set -eu

require_env() {
	var_name=$1
	eval "var_value=\${$var_name:-}"
	[ -n "$var_value" ] || {
		printf '%s\n' "Missing required environment variable: $var_name" >&2
		exit 1
	}
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || {
		printf '%s\n' "Missing required command: $1" >&2
		exit 1
	}
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			printf '%s\n' "$value_name must be a positive integer: $value" >&2
			exit 1
			;;
	esac

	[ "$value" -gt 0 ] || {
		printf '%s\n' "$value_name must be a positive integer: $value" >&2
		exit 1
	}
}

assert_file() {
	[ -f "$1" ] || {
		printf '%s\n' "Missing expected file: $1" >&2
		exit 1
	}
}

copy_docs() {
	package_name=$1
	stage_dir=$2
	doc_dir="$stage_dir/usr/share/doc/$package_name"

	mkdir -p "$doc_dir"
	cp README.md "$doc_dir/"
	cp README_ja.md "$doc_dir/"
}

write_control_file() {
	control_path=$1
	package_name=$2
	package_description=$3
	package_section=$4
	depends_value=$5

	{
		printf 'Package: %s\n' "$package_name"
		printf 'Version: %s\n' "$CDRIP_PACKAGE_VERSION"
		printf 'Section: %s\n' "$package_section"
		printf 'Priority: optional\n'
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Maintainer: %s\n' "$CDRIP_PACKAGE_MAINTAINER"
		if [ -n "$depends_value" ]; then
			printf 'Depends: %s\n' "$depends_value"
		fi
		printf 'Description: %s\n' "$package_description"
	} >"$control_path"
}

calculate_shlibdeps() {
	target_path=$1
	package_name=$2
	tmp_dir=$(mktemp -d)

	mkdir -p "$tmp_dir/debian"
	cat >"$tmp_dir/debian/control" <<EOF
Source: $package_name
Section: misc
Priority: optional
Maintainer: $CDRIP_PACKAGE_MAINTAINER
Standards-Version: 4.6.2

Package: $package_name
Architecture: $deb_arch
Description: temporary package metadata for dependency calculation
EOF

	depends_value=$(cd "$tmp_dir" && dpkg-shlibdeps -O "$target_path" | sed -n 's/^shlibs:Depends=//p')
	rm -rf "$tmp_dir"

	[ -n "$depends_value" ] || {
		printf '%s\n' "dpkg-shlibdeps failed to calculate runtime dependencies for $target_path" >&2
		exit 1
	}

	printf '%s\n' "$depends_value"
}

stage_lib_package() {
	stage_dir=$work_dir/stage/$CDRIP_LIB_PACKAGE_NAME
	lib_dir="$stage_dir/usr/lib/$multiarch"
	include_dir="$stage_dir/usr/include/cdrip"
	control_dir="$stage_dir/DEBIAN"

	rm -rf "$stage_dir"
	mkdir -p "$lib_dir" "$include_dir" "$control_dir"

	cp "$build_dir/libcdrip.so" "$lib_dir/"
	cp "$build_dir/libcdrip.a" "$lib_dir/"
	cp include/cdrip/* "$include_dir/"
	copy_docs "$CDRIP_LIB_PACKAGE_NAME" "$stage_dir"

	lib_depends=$(calculate_shlibdeps "$lib_dir/libcdrip.so" "$CDRIP_LIB_PACKAGE_NAME")
	write_control_file \
		"$control_dir/control" \
		"$CDRIP_LIB_PACKAGE_NAME" \
		"$CDRIP_LIB_PACKAGE_DESCRIPTION" \
		'libs' \
		"$lib_depends"
}

stage_cli_package() {
	stage_dir=$work_dir/stage/$CDRIP_CLI_PACKAGE_NAME
	bin_dir="$stage_dir/usr/bin"
	control_dir="$stage_dir/DEBIAN"

	rm -rf "$stage_dir"
	mkdir -p "$bin_dir" "$control_dir"

	cp "$build_dir/cdrip" "$bin_dir/"
	copy_docs "$CDRIP_CLI_PACKAGE_NAME" "$stage_dir"

	cli_depends=$(calculate_shlibdeps "$bin_dir/cdrip" "$CDRIP_CLI_PACKAGE_NAME")
	write_control_file \
		"$control_dir/control" \
		"$CDRIP_CLI_PACKAGE_NAME" \
		"$CDRIP_CLI_PACKAGE_DESCRIPTION" \
		'sound' \
		"$cli_depends"
}

require_env CDRIP_WORK_DIR
require_env CDRIP_META_DIR
require_env CDRIP_HOST_UID
require_env CDRIP_HOST_GID
require_env CDRIP_PACKAGE_VERSION
require_env CDRIP_LIB_PACKAGE_NAME
require_env CDRIP_CLI_PACKAGE_NAME
require_env CDRIP_LIB_PACKAGE_DESCRIPTION
require_env CDRIP_CLI_PACKAGE_DESCRIPTION
require_env CDRIP_PACKAGE_MAINTAINER
require_env CDRIP_BUILD_TYPE

require_command apt-get

CDRIP_MAKE_JOBS=${CDRIP_MAKE_JOBS:-1}
validate_positive_integer 'CDRIP_MAKE_JOBS' "$CDRIP_MAKE_JOBS"

work_dir=$CDRIP_WORK_DIR
meta_dir=$CDRIP_META_DIR
build_dir="$work_dir/build"

rm -rf "$work_dir" "$meta_dir"
mkdir -p "$build_dir" "$meta_dir"

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
	cmake \
	dpkg-dev \
	libcdio-paranoia-dev \
	libcddb2-dev \
	libebur128-dev \
	libchafa-dev \
	libflac++-dev \
	libglib2.0-dev \
	libjpeg-dev \
	libjson-glib-dev \
	liblcms2-dev \
	libpng-dev \
	libsoup-3.0-dev \
	pkg-config

require_command cmake
require_command dpkg-architecture
require_command dpkg-shlibdeps

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE="$CDRIP_BUILD_TYPE"
cmake --build "$build_dir" --parallel "$CDRIP_MAKE_JOBS"

assert_file "$build_dir/libcdrip.so"
assert_file "$build_dir/libcdrip.a"
assert_file "$build_dir/cdrip"

deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

stage_lib_package
stage_cli_package

printf '%s\n' "$deb_arch" >"$meta_dir/deb_arch"
printf '%s\n' "$multiarch" >"$meta_dir/multiarch"

chown -R "$CDRIP_HOST_UID:$CDRIP_HOST_GID" "$work_dir" "$meta_dir"
