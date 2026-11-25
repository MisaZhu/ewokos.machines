/* host shim: ga10b_falcon.c only needs the DMA-load address lookup, which
 * the host tests never reach */
#ifndef SHIM_EWOKSYS_DMA_H
#define SHIM_EWOKSYS_DMA_H
#include <stdint.h>
static inline uint64_t dma_phy_addr(int block, void* va) {
    (void)block;
    (void)va;
    return 0;
}
#endif
