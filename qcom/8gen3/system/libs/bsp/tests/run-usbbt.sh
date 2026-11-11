#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../../../.."
out=$(mktemp -d "${TMPDIR:-/tmp}/q8b-usbbt.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
${CC:-clang} -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I machines/qcom/8gen3/system/libs/bsp/include \
    -I system/gui/libs/usb/include \
    -I system/basic/libc/libewoksys/ewoksys/include \
    -I kernel/kernel/include -I kernel/lib/include \
    -idirafter system/build_aarch64/8gen3/include \
    machines/qcom/8gen3/system/libs/bsp/tests/usbbt_test.c -o "$out/usbbt_test"
"$out/usbbt_test"
${CC:-clang} -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I system/gui/libs/bt/include \
    -I system/basic/libc/libewoksys/ewoksys/include \
    -I kernel/kernel/include -I kernel/lib/include \
    -idirafter system/build_aarch64/8gen3/include \
    machines/qcom/8gen3/system/libs/bsp/tests/bt_client_test.c -o "$out/bt_client_test"
"$out/bt_client_test"
