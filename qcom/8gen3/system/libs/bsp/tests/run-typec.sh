#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../../../.."
out=$(mktemp -d "${TMPDIR:-/tmp}/q8b-typec.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
case $(uname -s) in
    Darwin) gc_sections=-Wl,-dead_strip ;;
    *) gc_sections=-Wl,--gc-sections ;;
esac
${CC:-clang} -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections "$gc_sections" \
    -I machines/qcom/8gen3/system/libs/bsp/include \
    -I system/gui/libs/usb/include \
    -I system/basic/libc/libewoksys/ewoksys/include \
    -I kernel/kernel/include -I kernel/lib/include \
    -idirafter system/build_aarch64/8gen3/include \
    machines/qcom/8gen3/system/libs/bsp/tests/typec_test.c -o "$out/typec_test"
"$out/typec_test"
