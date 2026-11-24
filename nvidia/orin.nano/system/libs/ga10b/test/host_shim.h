#ifndef HOST_SHIM_H
#define HOST_SHIM_H

#include <stdint.h>

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

#endif