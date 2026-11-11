#!/bin/sh
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
Q8B_DIR=$(CDPATH= cd -- "$TEST_DIR/../../../.." && pwd)
REPO_DIR=$(CDPATH= cd -- "$Q8B_DIR/../.." && pwd)
TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/q8b-adreno-rpmh.XXXXXX")
trap 'rm -rf "$TEST_OUT"' EXIT HUP INT TERM
"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$Q8B_DIR/include" -I"$TEST_DIR/../include" \
    -I"$REPO_DIR/system/basic/libc/libewoksys/ewoksys/include" \
    -I"$REPO_DIR/kernel/kernel/include" \
    -I"$REPO_DIR/kernel/platform/aarch64/arch/common/include" \
    "$TEST_DIR/rpmh_write_test.c" -o "$TEST_OUT/rpmh_write_test"
"$TEST_OUT/rpmh_write_test"
