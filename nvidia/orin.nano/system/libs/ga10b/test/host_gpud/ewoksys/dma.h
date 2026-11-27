#ifndef HOST_GPUD_DMA_H
#define HOST_GPUD_DMA_H
#include <stdint.h>
#include <ewoksys/ewokdef.h>
ewokos_addr_t dma_alloc(int32_t dma_block_id, uint32_t size);
void          dma_free(int32_t dma_block_id, ewokos_addr_t vaddr);
ewokos_addr_t dma_phy_addr(int32_t dma_block_id, ewokos_addr_t vaddr);
#endif