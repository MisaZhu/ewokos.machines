#ifndef Q8B_BSP_SPI_H
#define Q8B_BSP_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define Q8B_SPI_CPHA       1U
#define Q8B_SPI_CPOL       2U
#define Q8B_SPI_LSB_FIRST  4U
#define Q8B_SPI_LOOPBACK   8U
#define Q8B_SPI_MAX_XFER   6144U  /* Himax event-stack frame 5132B + header */
/* Assert chip select from software instead of letting the serial engine drive
 * the CS pad.  The pad is then left as a plain GPIO and the caller is
 * responsible for its level; GENI only owns MISO/MOSI/CLK.  Needed on boards
 * where the engine's CS demux cannot be routed to the touch IC. */
#define Q8B_SPI_SW_CS      16U

/* Polled, eight-bit GENI master. Currently supports TW220 SPI22, CS0.
 * One process must own the bus; callers serialize complete transactions.
 * A transfer asserts CS for all bytes and deasserts it before returning.
 * Buffers can alias. NULL TX selects RX-only; NULL RX selects TX-only.
 * A transfer is full length in both directions: the TX FIFO is driven for
 * every byte (0xff where the caller supplies no TX data), so an RX-only read
 * and a full-duplex read with an all-0xff TX buffer produce the same wire
 * traffic.  This matters on the Himax touch path, whose register-read header
 * is 3 bytes but whose frame is 5 KiB.
 * No DMA or GPIO transfer fallback is performed. */
typedef struct {
    uintptr_t gcc, se, wrapper, tlmm;
    uint32_t se_pa;                  /* physical SE base of the active bus */
    uint32_t source_hz, speed_hz, requested_hz, clock_index;
    uint32_t fifo_depth, rx_depth, oversampling;
    uint32_t hw_version, firmware;
    uint32_t last_irq, last_status;
    size_t sent, received;
    const char* stage;
    bool ready;
    bool sw_cs;                      /* CS pad is owned by the caller */
} q8b_spi_t;

/* Prepare/check GCC clocks only; does not access GENI or change pinmux.
 * Shared QUP votes are preserved and never switched off. */
int q8b_spi_prepare(q8b_spi_t* spi, unsigned bus, unsigned speed_hz);
int q8b_spi_open(q8b_spi_t* spi, unsigned bus, unsigned speed_hz, unsigned mode);
int q8b_spi_transfer(q8b_spi_t* spi, const void* tx, void* rx, size_t len);

/* Read-only transfer through the serial engine's own DMA, which needs no GSI
 * channel or SMMU route.  rx must live in physically contiguous memory and
 * rx_pa is its physical address.  Returns 0 on success; the engine is left
 * unusable (spi->ready false) on failure, like the polled path. */
int q8b_spi_transfer_dma_read(q8b_spi_t* spi, void* rx, size_t len,
        uint64_t rx_pa, uint32_t timeout_us);

#endif
