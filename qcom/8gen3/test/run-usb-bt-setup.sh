#!/bin/sh
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$TEST_DIR/../../.." && pwd)
TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/q8b-usbbt.XXXXXX")
trap 'rm -rf "$TEST_OUT"' EXIT HUP INT TERM
mkdir -p "$TEST_OUT/bsp" "$TEST_OUT/ewoksys"
cat > "$TEST_OUT/bsp/bsp_usb.h" <<'HEADER'
#ifndef USB_TEST_H
#define USB_TEST_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { int unused; } bsp_usb_dev_t;
typedef struct { int unused; } vdevice_t;
typedef struct { int count, ints[8]; } proto_t;
typedef struct proto_factor {
    struct proto_factor *(*addi)(proto_t *,int);
    struct proto_factor *(*add)(proto_t *,const void *,uint32_t);
} proto_factor_t;
proto_factor_t *get_proto_factor(void);
#define PF get_proto_factor()
int proto_read_int(proto_t *);
void *proto_read(proto_t *,int32_t *);
typedef struct { uint8_t bmRequestType,bRequest; uint16_t wValue,wIndex,wLength; } usb_setup_pkt_t;
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5
int bsp_usb_bt_cntl(vdevice_t *,int,int,proto_t *,proto_t *);
int bsp_usb_control_xfer(bsp_usb_dev_t *,usb_setup_pkt_t *,void *,bool);
int bsp_usb_bulk_xfer(bsp_usb_dev_t *,uint8_t,void *,int,bool);
int bsp_usb_int_in_poll(bsp_usb_dev_t *,uint8_t,void *,int);
int bsp_usb_bulk_in_poll(bsp_usb_dev_t *,uint8_t,void *,int);
int bsp_usb_int_in_open(bsp_usb_dev_t *,uint8_t,uint16_t,uint8_t);
int bsp_usb_bulk_open(bsp_usb_dev_t *,uint8_t,uint16_t);
bool bsp_usb_xhci_owned(bsp_usb_dev_t *);
HEADER
printf '#endif\n' >> "$TEST_OUT/bsp/bsp_usb.h"
printf '#include <stdint.h>\nuint64_t kernel_tic_ms(int);\n' > "$TEST_OUT/ewoksys/kernel_tic.h"
printf 'void klog(const char *, ...);\n' > "$TEST_OUT/ewoksys/klog.h"
"${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -g -O1 -Wall -Wextra -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$TEST_OUT" \
    -I"$REPO_DIR/system/basic/libc/libewoksys/ewoksys/include" \
    -DUSB_BT_SOURCE="\"${USB_BT_SOURCE:-$TEST_DIR/../system/libs/bsp/src/bsp_usbbt.c}\"" \
    "$TEST_DIR/usb-bt-setup-test.c" -o "$TEST_OUT/test"
"$TEST_OUT/test" "$TEST_OUT/rampatch.bin"
