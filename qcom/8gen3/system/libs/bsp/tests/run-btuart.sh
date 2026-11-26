#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../../../.."
out=$(mktemp -d "${TMPDIR:-/tmp}/qcom8gen3-btuart.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
${CC:-clang} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I system/gui/libs/bt/include \
    machines/qcom/8gen3/system/libs/bsp/tests/btuart_test.c -o "$out/btuart_test"
"$out/btuart_test"