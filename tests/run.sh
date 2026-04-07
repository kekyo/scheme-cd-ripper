#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for test_script in "$ROOT_DIR"/tests/test_*.sh; do
	[ -f "$test_script" ] || continue
	"$test_script"
done
