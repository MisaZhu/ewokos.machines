#!/bin/sh
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QCOM8GEN3_DIR=$(CDPATH= cd -- "$TEST_DIR/../../../.." && pwd)
REPO_DIR=$(CDPATH= cd -- "$QCOM8GEN3_DIR/../.." && pwd)
TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/qcom8gen3-adreno-rpmh.XXXXXX")
trap 'rm -rf "$TEST_OUT"' EXIT HUP INT TERM
"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$QCOM8GEN3_DIR/include" -I"$TEST_DIR/../include" \
    -I"$REPO_DIR/system/basic/libc/libewoksys/ewoksys/include" \
    -I"$REPO_DIR/kernel/kernel/include" \
    -I"$REPO_DIR/kernel/platform/aarch64/arch/common/include" \
    "$TEST_DIR/rpmh_write_test.c" -o "$TEST_OUT/rpmh_write_test"
"$TEST_OUT/rpmh_write_test"
