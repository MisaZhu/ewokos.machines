#!/usr/bin/env python3
"""Host RX regression using the real BSP; optional arg selects a prior source.

MMIO/DMA are mocked. ARM dsb instructions become compiler fences in this
host-only copy; actual PCIe/DMA ordering requires the board bulk test.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else here.parent / 'system/libs/bsp/src/bsp_eth.c'
abi = '''#pragma once
#include <stdint.h>
typedef uint64_t ewokos_addr_t;
#define SYS_MEM_MAP 1
ewokos_addr_t dma_alloc(int, uint32_t);
ewokos_addr_t dma_phy_addr(int, ewokos_addr_t);
ewokos_addr_t syscall3(int, ewokos_addr_t, ewokos_addr_t, ewokos_addr_t);
uint32_t get32(ewokos_addr_t);
void put32(ewokos_addr_t, uint32_t);
void klog(const char *, ...);
'''
with tempfile.TemporaryDirectory(prefix='q8b-eth-rx-') as work:
    work = Path(work)
    (work / 'ewoksys').mkdir()
    (work / 'abi.h').write_text(abi)
    for name in ['dma', 'klog', 'mmio', 'syscall']:
        (work / 'ewoksys' / (name + '.h')).write_text('#include "abi.h"\n')
    native = work / 'bsp_eth.c'
    native.write_text(source.read_text().replace('"dsb sy"', '""'))
    binary = work / 'test'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(work),
                    '-I', str(here.parent / 'system/libs/bsp/include'),
                    '-DDRIVER_SOURCE="' + str(native) + '"',
                    str(here / 'eth_rx_test.c'), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
