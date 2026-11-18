#!/bin/bash
set -euo pipefail

# Include the actual files used for this build, including submodule contents.
# The manifest also lets an unpacked source release be packaged without Git.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_PATH=$(realpath -m "${1:?Usage: create_source_archive.sh output.tar.xz}")
STAGE_DIR=$(mktemp -d)
trap 'rm -rf "$STAGE_DIR"' EXIT
test -f "$ROOT_DIR/deps/chafa/configure.ac" || {
    echo 'Chafa sources are missing. Run: git submodule update --init --recursive' >&2
    exit 1
}
if [ -f "$ROOT_DIR/.cdrip-source-files" ]; then
    cp "$ROOT_DIR/.cdrip-source-files" "$STAGE_DIR/.cdrip-source-files"
else
    git -C "$ROOT_DIR" ls-files --cached --recurse-submodules -z >"$STAGE_DIR/.cdrip-source-files"
fi
tar -C "$ROOT_DIR" --null --verbatim-files-from --no-recursion \
    -T "$STAGE_DIR/.cdrip-source-files" -cf - | tar -C "$STAGE_DIR" -xf -
mkdir -p "$(dirname "$OUTPUT_PATH")"
tar -C "$STAGE_DIR" -cJf "$OUTPUT_PATH" .
printf 'Source archive: %s\n' "$OUTPUT_PATH"
