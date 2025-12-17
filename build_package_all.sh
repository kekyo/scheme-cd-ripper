#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFRESH_BASE=0

usage() {
    cat <<EOF
Usage: ${0} [--refresh-base]
  --refresh-base   Recreate cowbuilder bases for all combinations
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --refresh-base)
            REFRESH_BASE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

COMBINATIONS=(
    "ubuntu noble amd64"
    "ubuntu noble arm64"
#   "ubuntu noble i386"
#   "ubuntu noble armhf"
    "ubuntu jammy amd64"
    "ubuntu jammy arm64"
#   "ubuntu jammy i386"
#   "ubuntu jammy armhf"
    "debian bookworm amd64"
    "debian bookworm arm64"
    "debian bookworm i386"
    "debian bookworm armhf"
    "debian trixie amd64"
    "debian trixie arm64"
    "debian trixie i386"
    "debian trixie armhf"
)

for combo in "${COMBINATIONS[@]}"; do
    read -r distro release arch <<<"${combo}"
    echo "=== Building ${distro} ${release} ${arch} ==="
    cmd=("${ROOT_DIR}/build_package.sh" --distro "${distro}" --release "${release}" --arch "${arch}")
    if [[ "${REFRESH_BASE}" -eq 1 ]]; then
        cmd+=(--refresh-base)
    fi
    if ! "${cmd[@]}"; then
        echo "!! Failed: ${distro} ${release} ${arch}" >&2
        exit 1
    fi
done

echo "All combinations built successfully."
