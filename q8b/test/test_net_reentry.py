#!/usr/bin/env python3
"""Exercise real net.c with injected single-task IPC and VFS callbacks.

Optional argument selects another net.c (e.g. a pre-fix git export).
No board access or target compiler required.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else here.parent / 'system/drivers/net/net.c'
abi = '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
typedef struct { uint64_t node; } fsinfo_t;
typedef struct { int unused; } proto_t;
typedef struct { void (*add)(proto_t *, const void *, int); void (*addi)(proto_t *, int); } proto_funcs_t;
extern proto_funcs_t *PF;
typedef struct st_vdevice vdevice_t;
struct st_vdevice {
    char desc[64]; fsinfo_t mnt_info;
    int (*read)(vdevice_t *, int, int, fsinfo_t *, void *, int, off_t, void *);
    int (*write)(vdevice_t *, int, int, fsinfo_t *, const void *, int, off_t, void *);
    int (*dev_cntl)(vdevice_t *, int, int, proto_t *, proto_t *, void *);
    char *(*cmd)(vdevice_t *, int, int, char **, void *);
    uint32_t (*check_poll_events)(vdevice_t *, int, int, fsinfo_t *, void *);
    int (*loop_step)(vdevice_t *, void *);
};
#define VFS_ERR_RETRY (-128)
#define VFS_EVT_RD 1
#define VFS_EVT_WR 2
#define FS_TYPE_CHAR 1
int ipc_disable(void);
void ipc_enable(void);
int usleep(unsigned int);
int vfs_wakeup(uint64_t, int);
int device_run(vdevice_t *, const char *, int, int, bool);
void klog(const char *, ...);
'''
with tempfile.TemporaryDirectory(prefix='q8b-net-reentry-') as work:
    work = Path(work)
    (work / 'ewoksys').mkdir()
    (work / 'abi.h').write_text(abi)
    for name in ['ipc', 'klog', 'proc', 'proto', 'vdevice', 'vfs', 'vfsc']:
        (work / 'ewoksys' / (name + '.h')).write_text('#include "abi.h"\n')
    binary = work / 'test'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(work),
                    '-I', str(here.parent / 'system/libs/bsp/include'),
                    '-DDRIVER_SOURCE="' + str(source) + '"',
                    str(here / 'net_reentry_test.c'), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
