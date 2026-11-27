#ifndef HOST_GPUD_DMA_H
#define HOST_GPUD_DMA_H
#include <stdint.h>
#include <ewoksys/ewokdef.h>
ewokos_addr_t dma_alloc(uint32_t size);
void          dma_free(ewokos_addr_t vaddr);
ewokos_addr_t dma_phy_addr(ewokos_addr_t vaddr);
#endif