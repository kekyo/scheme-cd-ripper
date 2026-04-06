#!/bin/sh

set -eu

PROJECT_ROOT=${BUILD_PACKAGE_PROJECT_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
ARTIFACT_ROOT="$PROJECT_ROOT/artifacts"
DEB_ARTIFACT_ROOT="$ARTIFACT_ROOT/deb"
LIB_PACKAGE_NAME=libcdrip-dev
CLI_PACKAGE_NAME=cdrip
LIB_PACKAGE_DESCRIPTION="C++ CD ripping library development package."
CLI_PACKAGE_DESCRIPTION="CLI CD ripper using libcdrip."
DEFAULT_MAINTAINER="scheme-cd-ripper packager <packager@localhost>"
DEFAULT_PARALLEL_JOB_CAP=14

LINUX_MATRIX=$(cat <<'EOF'
debian bookworm x86_64 linux/amd64
debian bookworm i686 linux/386
debian bookworm arm64 linux/arm64
debian bookworm armv7l linux/arm/v7
debian trixie x86_64 linux/amd64
debian trixie i686 linux/386
debian trixie arm64 linux/arm64
debian trixie armv7l linux/arm/v7
debian trixie riscv64 linux/riscv64
ubuntu 22.04 x86_64 linux/amd64
ubuntu 22.04 arm64 linux/arm64
ubuntu 24.04 x86_64 linux/amd64
ubuntu 24.04 arm64 linux/arm64
EOF
)

print_usage() {
	cat <<'EOF'
Usage: ./build_package.sh [options]

Options:
  --version <version>  Package version. Defaults to a screw-up-derived version.
  --target <target>    all or deb. Defaults to all.
  --distro <list>      Comma-separated distro filter.
  --release <list>     Comma-separated release filter.
  --arch <list>        Comma-separated architecture filter.
  --jobs <count>       Maximum concurrent package jobs. Defaults to auto (up to 14).
  --debug              Build packages with a Debug CMake configuration.
  --print-version      Print the resolved package version and exit.
  --help               Show this help.
EOF
}

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

assert_contains() {
	target_path=$1
	expected_text=$2

	grep -F "$expected_text" "$target_path" >/dev/null 2>&1 || fail "Missing expected text in $target_path: $expected_text"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			fail "$value_name must be a positive integer: $value"
			;;
	esac

	[ "$value" -gt 0 ] || fail "$value_name must be a positive integer: $value"
}

detect_processor_count() {
	detected_count=''

	if command -v getconf >/dev/null 2>&1; then
		detected_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v nproc >/dev/null 2>&1; then
		detected_count=$(nproc 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v sysctl >/dev/null 2>&1; then
		detected_count=$(sysctl -n hw.ncpu 2>/dev/null || true)
	fi

	case $detected_count in
		'' | *[!0-9]*)
			detected_count=1
			;;
	esac

	if [ "$detected_count" -lt 1 ]; then
		detected_count=1
	fi

	printf '%s\n' "$detected_count"
}

min_int() {
	left_value=$1
	right_value=$2

	if [ "$left_value" -le "$right_value" ]; then
		printf '%s\n' "$left_value"
	else
		printf '%s\n' "$right_value"
	fi
}

detect_version() {
	require_command screw-up
	detected_version=$(printf '%s\n' '{version}' | screw-up format | tr -d '\r' | head -n 1)
	[ -n "$detected_version" ] || fail 'screw-up did not return a version'
	printf '%s\n' "$detected_version"
}

validate_version() {
	case $1 in
		'' | *[!0-9A-Za-z.+:~\-]*)
			fail "Invalid package version: $1"
			;;
	esac
}

canonical_distro() {
	value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')

	case $value in
		debian | ubuntu)
			printf '%s\n' "$value"
			;;
		*)
			fail "Unsupported distro filter: $1"
			;;
	esac
}

canonical_release() {
	value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')

	case $value in
		bookworm | trixie | 22.04 | 24.04)
			printf '%s\n' "$value"
			;;
		jammy)
			printf '%s\n' '22.04'
			;;
		noble)
			printf '%s\n' '24.04'
			;;
		*)
			fail "Unsupported release filter: $1"
			;;
	esac
}

canonical_arch() {
	value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')

	case $value in
		x86_64 | amd64)
			printf '%s\n' 'x86_64'
			;;
		i686 | i386 | i486 | i586 | x86)
			printf '%s\n' 'i686'
			;;
		arm64 | aarch64)
			printf '%s\n' 'arm64'
			;;
		armv7l | armv7 | armhf)
			printf '%s\n' 'armv7l'
			;;
		riscv64)
			printf '%s\n' 'riscv64'
			;;
		*)
			fail "Unsupported architecture filter: $1"
			;;
	esac
}

normalize_filter_list() {
	filter_kind=$1
	filter_value=$2

	if [ -z "$filter_value" ]; then
		printf '\n'
		return 0
	fi

	previous_ifs=$IFS
	IFS=','
	normalized=''
	for filter_item in $filter_value; do
		case $filter_kind in
			distro)
				resolved_filter=$(canonical_distro "$filter_item")
				;;
			release)
				resolved_filter=$(canonical_release "$filter_item")
				;;
			arch)
				resolved_filter=$(canonical_arch "$filter_item")
				;;
			*)
				IFS=$previous_ifs
				fail "Unsupported filter kind: $filter_kind"
				;;
		esac

		normalized="${normalized}${normalized:+,}$resolved_filter"
	done
	IFS=$previous_ifs

	printf '%s\n' "$normalized"
}

matches_filter() {
	filter_value=$1
	actual_value=$2
	if [ -z "$filter_value" ]; then
		return 0
	fi

	previous_ifs=$IFS
	IFS=','
	for allowed_value in $filter_value; do
		if [ "$allowed_value" = "$actual_value" ]; then
			IFS=$previous_ifs
			return 0
		fi
	done
	IFS=$previous_ifs
	return 1
}

count_deb_builds() {
	build_count=0

	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		build_count=$((build_count + 1))
	done <<EOF
$LINUX_MATRIX
EOF

	printf '%s\n' "$build_count"
}

count_matching_files() {
	target_dir=$1
	pattern=$2

	if [ ! -d "$target_dir" ]; then
		printf '%s\n' '0'
		return 0
	fi

	find "$target_dir" -type f -name "$pattern" | wc -l | tr -d ' '
}

expected_elf_class() {
	case $1 in
		x86_64 | arm64 | riscv64)
			printf '%s\n' 'ELF64'
			;;
		i686 | armv7l)
			printf '%s\n' 'ELF32'
			;;
		*)
			fail "Unsupported ELF class lookup: $1"
			;;
	esac
}

expected_elf_machine() {
	case $1 in
		x86_64)
			printf '%s\n' 'Advanced Micro Devices X86-64'
			;;
		i686)
			printf '%s\n' 'Intel 80386'
			;;
		arm64)
			printf '%s\n' 'AArch64'
			;;
		armv7l)
			printf '%s\n' 'ARM'
			;;
		riscv64)
			printf '%s\n' 'RISC-V'
			;;
		*)
			fail "Unsupported ELF machine lookup: $1"
			;;
	esac
}

deb_arch_name() {
	case $1 in
		x86_64)
			printf '%s\n' 'amd64'
			;;
		i686)
			printf '%s\n' 'i386'
			;;
		arm64)
			printf '%s\n' 'arm64'
			;;
		armv7l)
			printf '%s\n' 'armhf'
			;;
		riscv64)
			printf '%s\n' 'riscv64'
			;;
		*)
			fail "Unsupported Debian architecture lookup: $1"
			;;
	esac
}

deb_artifact_path() {
	package_name=$1
	distro=$2
	release=$3
	arch=$4
	deb_arch=$(deb_arch_name "$arch")

	printf '%s\n' "$DEB_ARTIFACT_ROOT/${package_name}-${VERSION}-${distro}-${release}-${deb_arch}.deb"
}

choose_container_engine() {
	if [ -n "${CONTAINER_ENGINE:-}" ]; then
		require_command "$CONTAINER_ENGINE"
		printf '%s\n' "$CONTAINER_ENGINE"
		return 0
	fi

	require_command podman
	printf '%s\n' 'podman'
}

container_image_for_target() {
	distro=$1
	release=$2
	arch=$3

	if [ "$arch" = 'riscv64' ]; then
		printf 'docker.io/library/%s:%s\n' "$distro" "$release"
		return 0
	fi

	case $arch in
		x86_64)
			repository_prefix='amd64'
			;;
		i686)
			repository_prefix='i386'
			;;
		arm64)
			repository_prefix='arm64v8'
			;;
		armv7l)
			repository_prefix='arm32v7'
			;;
		*)
			fail "Unsupported Debian package architecture: $arch"
			;;
	esac

	printf 'docker.io/%s/%s:%s\n' "$repository_prefix" "$distro" "$release"
}

build_deb_packages() {
	distro=$1
	release=$2
	arch=$3
	platform=$4
	image=$(container_image_for_target "$distro" "$release" "$arch")
	work_root="$TMP_ROOT/deb/$distro/$release/$arch"
	container_root="/workspace/artifacts/.tmp/$RUN_ID/deb/$distro/$release/$arch"

	printf '%s\n' "[deb] $distro $release $arch"
	rm -rf "$work_root"
	mkdir -p "$DEB_ARTIFACT_ROOT"

	"$CONTAINER_ENGINE_BIN" run --rm \
		--platform "$platform" \
		-v "$PROJECT_ROOT:/workspace" \
		-w /workspace \
		-e CDRIP_WORK_DIR="$container_root/work" \
		-e CDRIP_META_DIR="$container_root/meta" \
		-e CDRIP_HOST_UID="$(id -u)" \
		-e CDRIP_HOST_GID="$(id -g)" \
		-e CDRIP_PACKAGE_VERSION="$VERSION" \
		-e CDRIP_PACKAGE_COMMIT="unknown" \
		-e CDRIP_LIB_PACKAGE_NAME="$LIB_PACKAGE_NAME" \
		-e CDRIP_CLI_PACKAGE_NAME="$CLI_PACKAGE_NAME" \
		-e CDRIP_LIB_PACKAGE_DESCRIPTION="$LIB_PACKAGE_DESCRIPTION" \
		-e CDRIP_CLI_PACKAGE_DESCRIPTION="$CLI_PACKAGE_DESCRIPTION" \
		-e CDRIP_PACKAGE_MAINTAINER="${DEB_MAINTAINER:-$DEFAULT_MAINTAINER}" \
		-e CDRIP_BUILD_TYPE="$BUILD_TYPE" \
		-e CDRIP_MAKE_JOBS="$MAKE_JOBS" \
		"$image" \
		./scripts/build_linux_dist_container.sh

	dpkg-deb --root-owner-group --build \
		"$work_root/work/stage/$LIB_PACKAGE_NAME" \
		"$(deb_artifact_path "$LIB_PACKAGE_NAME" "$distro" "$release" "$arch")" >/dev/null
	dpkg-deb --root-owner-group --build \
		"$work_root/work/stage/$CLI_PACKAGE_NAME" \
		"$(deb_artifact_path "$CLI_PACKAGE_NAME" "$distro" "$release" "$arch")" >/dev/null
}

validate_lib_deb_package() {
	package_path=$1
	expected_arch=$2
	expected_deb_arch=$(deb_arch_name "$expected_arch")
	tmp_dir=$(mktemp -d)

	assert_file "$package_path"
	[ "$(dpkg-deb -f "$package_path" Architecture)" = "$expected_deb_arch" ] || fail "Unexpected Architecture field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Version)" = "$VERSION" ] || fail "Unexpected Version field in $package_path"
	[ -n "$(dpkg-deb -f "$package_path" Depends)" ] || fail "Missing Depends field in $package_path"

	dpkg-deb -x "$package_path" "$tmp_dir"

	for header_path in "$PROJECT_ROOT"/include/cdrip/*; do
		header_name=$(basename "$header_path")
		assert_file "$tmp_dir/usr/include/cdrip/$header_name"
	done

	assert_file "$tmp_dir/usr/share/doc/$LIB_PACKAGE_NAME/README.md"
	assert_file "$tmp_dir/usr/share/doc/$LIB_PACKAGE_NAME/README_ja.md"

	shared_lib=$(find "$tmp_dir/usr/lib" -type f -name 'libcdrip.so' | head -n 1)
	static_lib=$(find "$tmp_dir/usr/lib" -type f -name 'libcdrip.a' | head -n 1)
	[ -n "$shared_lib" ] || fail "Missing libcdrip.so in $package_path"
	[ -n "$static_lib" ] || fail "Missing libcdrip.a in $package_path"

	readelf -h "$shared_lib" >"$tmp_dir/readelf-lib.txt"
	assert_contains "$tmp_dir/readelf-lib.txt" "$(expected_elf_class "$expected_arch")"
	assert_contains "$tmp_dir/readelf-lib.txt" "$(expected_elf_machine "$expected_arch")"

	rm -rf "$tmp_dir"
}

validate_cli_deb_package() {
	package_path=$1
	expected_arch=$2
	expected_deb_arch=$(deb_arch_name "$expected_arch")
	tmp_dir=$(mktemp -d)

	assert_file "$package_path"
	[ "$(dpkg-deb -f "$package_path" Architecture)" = "$expected_deb_arch" ] || fail "Unexpected Architecture field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Version)" = "$VERSION" ] || fail "Unexpected Version field in $package_path"
	[ -n "$(dpkg-deb -f "$package_path" Depends)" ] || fail "Missing Depends field in $package_path"

	dpkg-deb -x "$package_path" "$tmp_dir"

	assert_file "$tmp_dir/usr/bin/cdrip"
	assert_file "$tmp_dir/usr/share/doc/$CLI_PACKAGE_NAME/README.md"
	assert_file "$tmp_dir/usr/share/doc/$CLI_PACKAGE_NAME/README_ja.md"

	readelf -h "$tmp_dir/usr/bin/cdrip" >"$tmp_dir/readelf-cli.txt"
	assert_contains "$tmp_dir/readelf-cli.txt" "$(expected_elf_class "$expected_arch")"
	assert_contains "$tmp_dir/readelf-cli.txt" "$(expected_elf_machine "$expected_arch")"

	rm -rf "$tmp_dir"
}

validate_deb_artifacts() {
	expected_count=$(( $(count_deb_builds) * 2 ))
	actual_count=$(count_matching_files "$DEB_ARTIFACT_ROOT" '*.deb')

	[ "$actual_count" = "$expected_count" ] || fail "Unexpected deb artifact count: $actual_count"
	[ "$expected_count" -gt 0 ] || return 0

	require_command readelf

	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		validate_lib_deb_package "$(deb_artifact_path "$LIB_PACKAGE_NAME" "$distro" "$release" "$arch")" "$arch"
		validate_cli_deb_package "$(deb_artifact_path "$CLI_PACKAGE_NAME" "$distro" "$release" "$arch")" "$arch"
	done <<EOF
$LINUX_MATRIX
EOF
}

validate_artifacts() {
	printf '%s\n' 'Validating generated artifacts'

	if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
		validate_deb_artifacts
	fi
}

wait_for_oldest_job() {
	[ "$ACTIVE_JOB_COUNT" -gt 0 ] || return 0

	set -- $ACTIVE_JOB_PIDS
	wait_pid=$1
	shift

	if wait "$wait_pid"; then
		:
	else
		JOB_FAILURE=1
	fi

	ACTIVE_JOB_PIDS=$*
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT - 1))
}

run_parallel_job() {
	while [ "$ACTIVE_JOB_COUNT" -ge "$PARALLEL_JOBS" ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'

	"$@" &
	ACTIVE_JOB_PIDS="${ACTIVE_JOB_PIDS}${ACTIVE_JOB_PIDS:+ }$!"
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT + 1))
}

wait_for_all_jobs() {
	while [ "$ACTIVE_JOB_COUNT" -gt 0 ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'
}

schedule_deb_builds() {
	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_deb_packages "$distro" "$release" "$arch" "$platform"
	done <<EOF
$LINUX_MATRIX
EOF
}

main() {
	VERSION=''
	TARGET='all'
	DISTRO_FILTER=''
	RELEASE_FILTER=''
	ARCH_FILTER=''
	PARALLEL_JOBS=''
	PRINT_VERSION='false'
	BUILD_TYPE='Release'

	while [ "$#" -gt 0 ]; do
		case $1 in
			--version)
				[ "$#" -ge 2 ] || fail 'Missing value for --version'
				VERSION=$2
				shift 2
				;;
			--target)
				[ "$#" -ge 2 ] || fail 'Missing value for --target'
				TARGET=$2
				shift 2
				;;
			--distro)
				[ "$#" -ge 2 ] || fail 'Missing value for --distro'
				DISTRO_FILTER=$(normalize_filter_list distro "$2")
				shift 2
				;;
			--release)
				[ "$#" -ge 2 ] || fail 'Missing value for --release'
				RELEASE_FILTER=$(normalize_filter_list release "$2")
				shift 2
				;;
			--arch)
				[ "$#" -ge 2 ] || fail 'Missing value for --arch'
				ARCH_FILTER=$(normalize_filter_list arch "$2")
				shift 2
				;;
			--jobs)
				[ "$#" -ge 2 ] || fail 'Missing value for --jobs'
				PARALLEL_JOBS=$2
				shift 2
				;;
			--debug)
				BUILD_TYPE='Debug'
				shift
				;;
			--help)
				print_usage
				exit 0
				;;
			--print-version)
				PRINT_VERSION='true'
				shift
				;;
			*)
				fail "Unknown argument: $1"
				;;
		esac
	done

	if [ -z "$VERSION" ]; then
		VERSION=$(detect_version)
	fi
	validate_version "$VERSION"
	if [ -n "$PARALLEL_JOBS" ]; then
		validate_positive_integer 'Parallel job count' "$PARALLEL_JOBS"
	fi

	if [ "$PRINT_VERSION" = 'true' ]; then
		printf '%s\n' "$VERSION"
		exit 0
	fi

	case $TARGET in
		all | deb)
			;;
		*)
			fail "Unsupported target: $TARGET"
			;;
	esac

	CPU_COUNT=$(detect_processor_count)
	if [ -z "$PARALLEL_JOBS" ]; then
		PARALLEL_JOBS=$(min_int "$CPU_COUNT" "$DEFAULT_PARALLEL_JOB_CAP")
	fi

	BUILD_TASK_COUNT=0
	if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
		BUILD_TASK_COUNT=$((BUILD_TASK_COUNT + $(count_deb_builds)))
	fi

	if [ "$BUILD_TASK_COUNT" -gt 0 ]; then
		EFFECTIVE_BUILD_JOBS=$(min_int "$PARALLEL_JOBS" "$BUILD_TASK_COUNT")
	else
		EFFECTIVE_BUILD_JOBS=1
	fi

	MAKE_JOBS=$((CPU_COUNT / EFFECTIVE_BUILD_JOBS))
	if [ "$MAKE_JOBS" -lt 1 ]; then
		MAKE_JOBS=1
	fi

	RUN_ID="run-$(date +%Y%m%d%H%M%S)-$$"
	TMP_ROOT="$ARTIFACT_ROOT/.tmp/$RUN_ID"
	ACTIVE_JOB_PIDS=''
	ACTIVE_JOB_COUNT=0
	JOB_FAILURE=0

	mkdir -p "$ARTIFACT_ROOT"
	rm -rf "$DEB_ARTIFACT_ROOT"
	mkdir -p "$TMP_ROOT"

	printf '%s\n' "Using up to $PARALLEL_JOBS package jobs with cmake --build --parallel $MAKE_JOBS"

	if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
		require_command dpkg-deb
		CONTAINER_ENGINE_BIN=$(choose_container_engine)
		schedule_deb_builds
	fi

	wait_for_all_jobs
	validate_artifacts

	printf '%s\n' "Artifacts generated in $ARTIFACT_ROOT"
}

if [ "${BUILD_PACKAGE_SOURCE_ONLY:-0}" = '1' ]; then
	return 0 2>/dev/null || exit 0
fi

main "$@"
