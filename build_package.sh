#!/bin/bash
set -euo pipefail

# Scheme CD music/sound ripper
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/scheme-cd-ripper

# One-shot Debian/Ubuntu package builder using cowbuilder (pbuilder) + qemu-user-static.
# This keeps the existing build.sh intact and runs it inside a minimal chroot.
# Each invocation handles a single distro/arch combination.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_NAME="$(basename "$0")"

usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} --distro <debian|ubuntu> --release <suite> --arch <arch> [options]
Options:
  --debug             Build with ./build.sh -d
  --mirror URL        Override apt mirror (defaults: deb.debian.org or archive.ubuntu.com)
  --refresh-base      Recreate the cowbuilder base even if it exists
  -h, --help          Show this help

Examples:
  ${SCRIPT_NAME} --distro debian --release bookworm --arch amd64
  ${SCRIPT_NAME} --distro ubuntu --release noble --arch arm64 --debug

Notes:
- Requires: cowbuilder (pbuilder), qemu-debootstrap, qemu-user-static, sudo/root.
- One combination per run; call repeatedly for the full matrix you need.
EOF
}

log() {
    echo "[build-package] $*"
}

normalize_arch() {
    case "$1" in
        amd64 | x86_64) echo "amd64" ;;
        i386 | i486 | i586 | i686 | x86) echo "i386" ;;
        armhf | armv7 | armv7l) echo "armhf" ;;
        arm64 | aarch64) echo "arm64" ;;
        *)
            log "Unsupported arch: $1 (use amd64/i386/armhf/arm64)"
            return 1
            ;;
    esac
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        log "Command not found: $1"
        exit 1
    fi
}

DISTRO=""
RELEASE=""
ARCH_INPUT=""
MIRROR=""
DEBUG_BUILD=0
REFRESH_BASE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --distro)
            DISTRO="$2"
            shift 2
            ;;
        --release)
            RELEASE="$2"
            shift 2
            ;;
        --arch)
            ARCH_INPUT="$2"
            shift 2
            ;;
        --mirror)
            MIRROR="$2"
            shift 2
            ;;
        --debug)
            DEBUG_BUILD=1
            shift
            ;;
        --refresh-base)
            REFRESH_BASE=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            log "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${DISTRO}" || -z "${RELEASE}" || -z "${ARCH_INPUT}" ]]; then
    usage
    exit 1
fi

DISTRO="$(echo "${DISTRO}" | tr '[:upper:]' '[:lower:]')"
case "${DISTRO}" in
    debian | ubuntu) ;;
    *)
        log "Unsupported distro: ${DISTRO} (use debian or ubuntu)"
        exit 1
        ;;
esac

DEB_ARCH="$(normalize_arch "${ARCH_INPUT}")"
if [[ -z "${DEB_ARCH}" ]]; then
    exit 1
fi

if [[ -z "${MIRROR}" ]]; then
    if [[ "${DISTRO}" == "ubuntu" ]]; then
        # Ubuntu keeps non-x86 architectures under ports.ubuntu.com.
        if [[ -n "${UBUNTU_MIRROR:-}" ]]; then
            MIRROR="${UBUNTU_MIRROR}"
        else
            case "${DEB_ARCH}" in
                amd64 | i386)
                    MIRROR="http://archive.ubuntu.com/ubuntu"
                    ;;
                *)
                    MIRROR="http://ports.ubuntu.com/ubuntu-ports"
                    ;;
            esac
        fi
    else
        MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
    fi
fi

COMPONENTS="main"
if [[ "${DISTRO}" == "ubuntu" ]]; then
    COMPONENTS="main universe"
fi

DEBOOTSTRAP_CMD="debootstrap"
if command -v qemu-debootstrap >/dev/null 2>&1; then
    DEBOOTSTRAP_CMD="qemu-debootstrap"
fi

DEBIAN_KEYRING="/usr/share/keyrings/debian-archive-keyring.gpg"
UBUNTU_KEYRING="/usr/share/keyrings/ubuntu-archive-keyring.gpg"

HOST_ARCH="$(dpkg --print-architecture)"
SUDO_BIN="sudo"
if [[ "${EUID}" -eq 0 ]]; then
    SUDO_BIN=""
fi

if [[ -n "${SUDO_BIN}" ]]; then
    require_cmd "${SUDO_BIN}"
fi
require_cmd cowbuilder
require_cmd "${DEBOOTSTRAP_CMD}"
require_cmd systemd-nspawn

if [[ "${HOST_ARCH}" != "${DEB_ARCH}" ]]; then
    if ! dpkg -s qemu-user-static >/dev/null 2>&1; then
        log "qemu-user-static is required for cross-chroot (install: sudo apt-get install -y qemu-user-static)"
        exit 1
    fi
fi

BASE_DIR="${PBUILDER_BASE_DIR:-${HOME}/.pbuilder}"
BASE_PATH="${BASE_DIR}/${DISTRO}-${RELEASE}-${DEB_ARCH}.cow"
BUILD_DIR_IN_CHROOT="/tmp/scheme-cd-ripper-${DISTRO}-${RELEASE}-${DEB_ARCH}-build"
OUTPUT_DIR="${SCRIPT_DIR}/artifacts/${DISTRO}-${RELEASE}-${DEB_ARCH}"
LOGFILE="${OUTPUT_DIR}/build.log"

init_logging() {
    mkdir -p "${OUTPUT_DIR}"
    : > "${LOGFILE}"
}

start_logging() {
    exec > >(tee -a "${LOGFILE}") 2>&1
}

cleanup_permissions() {
    if [[ -f "${LOGFILE}" || -d "${OUTPUT_DIR}" ]]; then
        if [[ -n "${SUDO_BIN}" ]]; then
            ${SUDO_BIN} chown -R "$(id -u):$(id -g)" "${OUTPUT_DIR}" 2>/dev/null || true
        else
            chown -R "$(id -u):$(id -g)" "${OUTPUT_DIR}" 2>/dev/null || true
        fi
    fi
}
trap cleanup_permissions EXIT

create_base() {
    log "Creating cowbuilder base: ${BASE_PATH} (${DISTRO} ${RELEASE} ${DEB_ARCH})"
    ${SUDO_BIN} mkdir -p "${BASE_DIR}"
    if [[ "${DISTRO}" == "debian" && ! -f "${DEBIAN_KEYRING}" ]]; then
        log "Installing debian-archive-keyring for debootstrap signature verification"
        ${SUDO_BIN} apt-get update
        ${SUDO_BIN} apt-get install -y debian-archive-keyring
    fi
    if [[ "${DISTRO}" == "ubuntu" && ! -f "${UBUNTU_KEYRING}" ]]; then
        log "Installing ubuntu-keyring for debootstrap signature verification"
        ${SUDO_BIN} apt-get update
        ${SUDO_BIN} apt-get install -y ubuntu-keyring
    fi
    local extra_pkgs=(
        build-essential
        cmake
        pkg-config
        libcdio-paranoia-dev
        libcddb2-dev
        libflac++-dev
        libglib2.0-dev
        libsoup-3.0-dev
        libjson-glib-dev
        dpkg-dev
        fakeroot
        devscripts
        nodejs
        npm
        git
        ca-certificates
        qemu-user-static
    )
    local keyring_opt=()
    if [[ "${DISTRO}" == "debian" ]]; then
        keyring_opt=(--debootstrapopts "--keyring=${DEBIAN_KEYRING}")
    elif [[ "${DISTRO}" == "ubuntu" ]]; then
        keyring_opt=(--debootstrapopts "--keyring=${UBUNTU_KEYRING}")
    fi
    ${SUDO_BIN} cowbuilder --create \
        --basepath "${BASE_PATH}" \
        --distribution "${RELEASE}" \
        --architecture "${DEB_ARCH}" \
        --mirror "${MIRROR}" \
        --components "${COMPONENTS}" \
        --debootstrap "${DEBOOTSTRAP_CMD}" \
        "${keyring_opt[@]}" \
        --extrapackages "$(printf '%s ' "${extra_pkgs[@]}")" \
        --debootstrapopts --variant=buildd
}

update_base() {
    log "Updating cowbuilder base: ${BASE_PATH}"
    ${SUDO_BIN} cowbuilder --update \
        --basepath "${BASE_PATH}" \
        --distribution "${RELEASE}" \
        --architecture "${DEB_ARCH}" \
        --mirror "${MIRROR}"
}

ensure_base() {
    if [[ "${REFRESH_BASE}" -eq 1 || ! -d "${BASE_PATH}" ]]; then
        if [[ "${REFRESH_BASE}" -eq 1 && -e "${BASE_PATH}" ]]; then
            log "Removing existing base path ${BASE_PATH}"
            ${SUDO_BIN} rm -rf "${BASE_PATH}"
        fi
        create_base
    else
        update_base
    fi
}

prepare_bind_mounts() {
    # Ensure mount points exist inside the chroot path.
    ${SUDO_BIN} mkdir -p "${BASE_PATH}${SCRIPT_DIR}"
    ${SUDO_BIN} mkdir -p "${BASE_PATH}${OUTPUT_DIR}"
}

run_build() {
    log "Building inside chroot (${DISTRO} ${RELEASE} ${DEB_ARCH})"
    local build_opts=""
    if [[ "${DEBUG_BUILD}" -eq 1 ]]; then
        build_opts="-d"
    fi

    # The inner script runs inside the chroot; variables are passed via env.
    local inner_script
    # read -d '' returns non-zero because no NUL delimiter; append || true to avoid set -e exit.
    read -r -d '' inner_script <<'EOS' || true
set -euo pipefail
set -x
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    curl \
    gnupg \
    libcdio-paranoia-dev \
    libcddb2-dev \
    libflac++-dev \
    libglib2.0-dev \
    libsoup-3.0-dev \
    libjson-glib-dev \
    dpkg-dev \
    fakeroot \
    devscripts \
    git \
    ca-certificates

# Install newer Node.js (>=20) using `n` to avoid distro/arch repo gaps.
apt-get install -y --no-install-recommends nodejs npm
npm install -g n
export PATH="/usr/local/bin:${PATH}"
n 22
hash -r
npm install -g screw-up

mkdir -p "${BUILD_DIR}"
cd "${REPO_DIR}"
echo "Starting build.sh in $(pwd) with ARCH=${DEB_ARCH} BUILD_DIR=${BUILD_DIR} BUILD_OPTS='${BUILD_OPTS}'"
set +e
ARCH="${DEB_ARCH}" BUILD_DIR="${BUILD_DIR}" ./build.sh ${BUILD_OPTS}
build_rc=$?
set -e
echo "build.sh finished with rc=${build_rc}"
mkdir -p "${OUTPUT_DIR}"
if ! compgen -G "${BUILD_DIR}/deb/"'*.deb' >/dev/null; then
    echo "No .deb found under ${BUILD_DIR}/deb"
    exit 1
fi
TARGET_SUFFIX="${DISTRO}-${RELEASE}-${DEB_ARCH}"
for pkg in "${BUILD_DIR}/deb/"*.deb; do
    base="$(basename "${pkg}")"
    pkg_name="${base%.deb}"
    pkg_version="$(dpkg-deb -f "${pkg}" Version || true)"
    if [[ -z "${pkg_version}" ]]; then
        pkg_version="unknown"
    fi
    renamed="${OUTPUT_DIR}/${pkg_name}-${TARGET_SUFFIX}-${pkg_version}.deb"
    cp -v "${pkg}" "${renamed}"
done
EOS

    # Use systemd-nspawn to avoid host glibc vs target mismatch.
    local copy_dir="/var/cache/pbuilder/build/nspawn-${DISTRO}-${RELEASE}-${DEB_ARCH}"
    ${SUDO_BIN} rm -rf "${copy_dir}"
    ${SUDO_BIN} cp -al "${BASE_PATH}" "${copy_dir}"
    log "Running systemd-nspawn inside ${copy_dir}"
    set +e
    ${SUDO_BIN} systemd-nspawn \
        --quiet \
        --machine "pb-${DISTRO}-${RELEASE}-${DEB_ARCH}-$$" \
        --directory "${copy_dir}" \
        --setenv "REPO_DIR=${SCRIPT_DIR}" \
        --setenv "BUILD_DIR=${BUILD_DIR_IN_CHROOT}" \
        --setenv "OUTPUT_DIR=${OUTPUT_DIR}" \
        --setenv "DEB_ARCH=${DEB_ARCH}" \
        --setenv "BUILD_OPTS=${build_opts}" \
        --setenv "DISTRO=${DISTRO}" \
        --setenv "RELEASE=${RELEASE}" \
        --bind "${SCRIPT_DIR}:${SCRIPT_DIR}" \
        --bind "${OUTPUT_DIR}:${OUTPUT_DIR}" \
        /bin/bash -lc "${inner_script}"
    local status=$?
    set -e
    ${SUDO_BIN} rm -rf "${copy_dir}"
    if [[ "${status}" -ne 0 ]]; then
        log "Build failed (exit ${status}). See ${LOGFILE}"
        exit "${status}"
    fi
    log "cowbuilder finished successfully."

    if [[ -n "${SUDO_BIN}" ]]; then
        ${SUDO_BIN} chown -R "$(id -u):$(id -g)" "${OUTPUT_DIR}"
    else
        chown -R "$(id -u):$(id -g)" "${OUTPUT_DIR}"
    fi
    log "Artifacts stored in ${OUTPUT_DIR}"
    log "Build log: ${LOGFILE}"
}

log "Target: distro=${DISTRO}, release=${RELEASE}, arch=${DEB_ARCH}"
log "Mirror: ${MIRROR}"

init_logging
start_logging
ensure_base
prepare_bind_mounts
run_build
