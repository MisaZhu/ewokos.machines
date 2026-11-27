#ifndef QCOM8GEN3_GPI_DMA_RT_H
#define QCOM8GEN3_GPI_DMA_RT_H
#include <bsp/gpi_dma.h>

/*
 * Userspace GPI DMA runtime (host tests only, not linked into libbsp).
 * Owns the ring buffers through the contiguous-slab shm + the DMA pin ABI:
 *
 *   gpi_rt_open()  : map the GPI instance, create pinned desc/event rings
 *   gpi_rt_start() : program context + allocate + start
 *   gpi_rt_xfer()  : build CONFIG0/GO/DMA TREs, doorbell, wait event
 *   gpi_rt_close() : stop (if started), dealloc, unpin, release buffers
 *
 * The pins keep the contiguous physical runs alive when every process detaches
 * before the hardware acknowledges; gpi_rt_close only unpins after stop/
 * dealloc.  One owner per instance; a poisoned dma handle must route to a
 * full reset, never a retry.  Host tests inject the io and shm backends.
 */
typedef struct {
    qcom8gen3_gpi_dma_t dma;
    qcom8gen3_gpi_rings_t rings;
    int desc_shm, ev_shm;
    uint32_t* desc_v;           /* descriptor ring, TRE_SIZE*entries */
    uint8_t*  ev_v;             /* event ring, EVENT_SIZE*entries */
    uintptr_t desc_pa, ev_pa;
    unsigned instance, entries, pinned;
    unsigned doorbells;
    const char* stage;
} qcom8gen3_gpi_rt_t;

/* Map the instance window and allocate the pinned contiguous rings. */
int qcom8gen3_gpi_rt_open(qcom8gen3_gpi_rt_t* rt, unsigned instance, unsigned entries);
int qcom8gen3_gpi_rt_start(qcom8gen3_gpi_rt_t* rt);
/* Full-duplex xfer: tx_len bytes out, rx_len bytes in (rx_len holds echoes /
 * reads).  Buffers are caller-owned kernel-style pinned shm or plain RAM
 * for the frames; the ring TREs carry the phys addresses given here. */
int qcom8gen3_gpi_rt_xfer(qcom8gen3_gpi_rt_t* rt, uint32_t tx_pa, uint32_t tx_hi,
                    uint32_t tx_len, uint32_t rx_pa, uint32_t rx_hi,
                    uint32_t rx_len, const qcom8gen3_gpi_spi_cfg_t* cfg,
                    uint64_t budget_us);
/* stop -> dealloc -> unpin -> detach shm. */
int qcom8gen3_gpi_rt_close(qcom8gen3_gpi_rt_t* rt);

#endif