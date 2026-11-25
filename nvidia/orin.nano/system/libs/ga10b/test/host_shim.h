#ifndef HOST_SHIM_H
#define HOST_SHIM_H

#include <stdint.h>

#include "ga10b.h"

/* host integration shim: mimics the ga10bd dry-run OS layer exactly
 * (ga10b_power.c rd/wr/poll semantics, zeroed DMA pool, file-backed fw)
 * while capturing every MMIO write for trace assertions. */

typedef struct { uint32_t off, val; } shim_wr_t;
#define SHIM_TRACE_CAP 131072u
extern shim_wr_t shim_trace[SHIM_TRACE_CAP];
extern uint32_t shim_nwr;
extern int shim_quiet;          /* 1 = suppress klog output */
extern uint32_t shim_dma_live;  /* allocs - frees (leak witness) */

void shim_reset(void);
/* first trace index with (off,val) at or after `from`, or -1 */
int shim_find_from(uint32_t from, uint32_t off, uint32_t val);
uint32_t shim_count_off(uint32_t off);
/* host VA of the live DMA buffer whose fake physical base is `phy` */
void* shim_dma_va(uint64_t phy);

/* ---- fake-hardware extensions (hw mode on a ga10b.bar0 array) ---- */
/* called after a register write was stored; `old` is the previous value */
extern void (*shim_wr_hook)(uint32_t off, uint32_t val, uint32_t old);
extern void (*shim_engine_hook)(uint32_t reset_id, int enable);
/* TOP device table to report; NULL = empty (the dry-run outcome) */
extern const ga10b_devtable_t* shim_devtable;
/* device-lifetime segment backing; NULL = a private zeroed block */
#define SHIM_PMEM_PHY 0x200000000ULL
extern void* shim_pmem;
/* process identity, and the only uuid ga10b_os_alive reports alive */
extern uint32_t shim_self_pid, shim_self_uuid, shim_alive_uuid;

#endif