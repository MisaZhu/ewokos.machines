#!/bin/sh
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$TEST_DIR/../../.." && pwd)
TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/q8b-wlan-boot.XXXXXX")
trap 'rm -rf "$TEST_OUT"' EXIT HUP INT TERM
mkdir -p "$TEST_OUT/ewoksys"
cat > "$TEST_OUT/ewoksys/vdevice.h" <<'HEADER'
#ifndef WLAN_HOST_VDEVICE_H
#define WLAN_HOST_VDEVICE_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
typedef uintptr_t ewokos_addr_t;
typedef struct { int unused; } fsinfo_t;
typedef struct { int unused; } proto_t;
typedef struct proto_factor {
    struct proto_factor *(*addi)(proto_t *,int);
    struct proto_factor *(*add)(proto_t *,const void *,uint32_t);
} proto_factor_t;
extern proto_factor_t *PF;
typedef struct vdevice {
    char desc[64];
    struct { ewokos_addr_t node; } mnt_info;
    int (*mounted)(struct vdevice *,ewokos_addr_t,void *);
    char *(*cmd)(struct vdevice *,int,int,char **,void *);
    int (*read)(struct vdevice *,int,int,fsinfo_t *,void *,int,off_t,void *);
    int (*write)(struct vdevice *,int,int,fsinfo_t *,const void *,int,off_t,void *);
    int (*dev_cntl)(struct vdevice *,int,int,proto_t *,proto_t *,void *);
    uint32_t (*check_poll_events)(struct vdevice *,int,int,fsinfo_t *,void *);
    int (*loop_step)(struct vdevice *,void *);
} vdevice_t;
#define VFS_EVT_RD 1
#define VFS_EVT_WR 2
#define FS_TYPE_CHAR 1
int dev_get_pid(const char *);
int device_run(vdevice_t *,const char *,int,int,bool);
void vfs_wakeup(ewokos_addr_t,unsigned);
HEADER
printf '#endif\n' >> "$TEST_OUT/ewoksys/vdevice.h"
printf '#include "vdevice.h"\n' > "$TEST_OUT/ewoksys/vfs.h"
printf '#include "vdevice.h"\n' > "$TEST_OUT/ewoksys/proto.h"
printf 'void ipc_disable(void);\nvoid ipc_enable(void);\n' > "$TEST_OUT/ewoksys/ipc.h"
printf '#include <stdint.h>\nuint64_t kernel_tic_ms(int);\n' > "$TEST_OUT/ewoksys/kernel_tic.h"
printf 'typedef struct str str_t;\n' > "$TEST_OUT/ewoksys/mstr.h"
case $(uname -s) in
    Darwin) GC_FLAG=-Wl,-dead_strip ;;
    *) GC_FLAG=-Wl,--gc-sections ;;
esac
"${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -g -O1 -Wall -Wextra \
    -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    "$GC_FLAG" -I"$TEST_OUT" -I"$TEST_DIR/../system/drivers/wlan" \
    -I"$REPO_DIR/system/basic/libc/libewoksys/ewoksys/include" \
    -I"$REPO_DIR/system/basic/libs/tinyjson/include" \
    -DWLAN_SOURCE="\"${WLAN_SOURCE:-$TEST_DIR/../system/drivers/wlan/wlan.c}\"" \
    "$TEST_DIR/wlan-boot-progress-test.c" -o "$TEST_OUT/test"
"$TEST_OUT/test"
