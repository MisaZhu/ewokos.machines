#ifndef Q8B_BSP_GPI_DMA_H
#define Q8B_BSP_GPI_DMA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Experimental QUP2 GPI DMA candidate; host tests only.  It is not linked
 * into libbsp or spitest: the TW220 SMMU has no route for GPI2 stream 0xb6
 * yet, and the interconnect/CX resources are not owned.  This module only
 * builds descriptor/event rings and decodes the GPI control plane on top of
 * a raw command layer, all through an injected io backend.
 *
 * TRE/event layout follows Qualcomm's GPI (qcom-gpi-dma): a 32-byte
 * transfer-ring element and a 16-byte event element.  SPI transfers are a
 * chained CONFIG0 -> GO -> DMA(TX|RX) sequence; a completion EVENT confirms
 * the whole chain.
 *
 * Lifecycle invariants (checked here, not by hardware):
 *  - allocate/start/stop/reset/dealloc order is enforced;
 *  - a doorbell may only be written after START and before STOP;
 *  - a stale or foreign IRQ/event is never consumed;
 *  - any command timeout or error poisons the handle;
 *  - rings must be power-of-two and use 32/16-byte elements.
 */
#define Q8B_GPI_TRE_SIZE     32u
#define Q8B_GPI_EVENT_SIZE   16u
#define Q8B_GPI_CH_MAX       2u
#define Q8B_GPI_REG_STRIDE   0x4000u

/* Context / command / IRQ / doorbell offsets relative to an instance's
 * mapped context base.  See README.md for the physical resource math. */
#define Q8B_GPI_CH_CNTXT0    0x000u
#define Q8B_GPI_CH_LENGTH    0x004u
#define Q8B_GPI_CH_BASE_LO   0x008u
#define Q8B_GPI_CH_BASE_HI   0x00cU
#define Q8B_GPI_CH_RP_LO     0x010u
#define Q8B_GPI_CH_WP_LO     0x018u
#define Q8B_GPI_EV_CNTXT0    0x000u
#define Q8B_GPI_EV_BASE_LO   0x008u
#define Q8B_GPI_EV_RP_LO     0x010u
#define Q8B_GPI_EV_WP_LO     0x018u
#define Q8B_GPI_CH_DOORBELL  0x22000u
#define Q8B_GPI_CH_CMD       0x23008u
#define Q8B_GPI_EV_CMD       0x23010u
#define Q8B_GPI_CH_IRQ_STS   0x23080u
#define Q8B_GPI_EV_IRQ_STS   0x23084u
#define Q8B_GPI_CH_IRQ_CLR   0x23088u
#define Q8B_GPI_EV_IRQ_CLR   0x2308cU

/* CNTXT0 bitfields. */
#define Q8B_GPI_CNTXT0_EL_SIZE 0xff000000u
#define Q8B_GPI_CNTXT0_CHSTATE 0x00f00000u
#define Q8B_GPI_CNTXT0_ERIDX   0x0007c000u
#define Q8B_GPI_CNTXT0_DIR     (1u << 3)
#define Q8B_GPI_CNTXT0_PROTO   0x7u
#define Q8B_GPI_CNTXT0_DMA_PROTO 0x2u
#define Q8B_GPI_EV_CNTXT0_INTYPE (1u << 16)
#define Q8B_GPI_EV_CNTXT0_CHTYPE 0xfu

/* TRE dword3 flags (type lives in bits 23:16 of dword3). */
#define Q8B_GPI_TRE_FLAGS_CHAIN  (1u << 0)
#define Q8B_GPI_TRE_FLAGS_IEOB   (1u << 8)
#define Q8B_GPI_TRE_FLAGS_IEOT   (1u << 9)
#define Q8B_GPI_TRE_FLAGS_BEI    (1u << 10)
#define Q8B_GPI_TRE_FLAGS_LINK   (1u << 11)
#define Q8B_GPI_TRE_FLAGS_TYPE   (0xffu << 16)
#define Q8B_GPI_TRE_TYPE_DMA       0x10u
#define Q8B_GPI_TRE_TYPE_IMMEDIATE 0x11u
#define Q8B_GPI_TRE_TYPE_GO        0x20u
#define Q8B_GPI_TRE_TYPE_CONFIG0   0x22u

/* SPI CONFIG0 dword0 / dword2; GO dword0 / dword2. */
#define Q8B_GPI_SPI_C0_WORD_SZ 0x1fu
#define Q8B_GPI_SPI_C0_LOOPBACK (1u << 8)
#define Q8B_GPI_SPI_C0_CS       (1u << 11)
#define Q8B_GPI_SPI_C0_CPHA     (1u << 12)
#define Q8B_GPI_SPI_C0_CPOL     (1u << 13)
#define Q8B_GPI_SPI_C0_TX_PACK  (1u << 24)
#define Q8B_GPI_SPI_C0_RX_PACK  (1u << 25)
#define Q8B_GPI_SPI_C0_CLK_DIV  0x00000fffu
#define Q8B_GPI_SPI_C0_CLK_SRC  (0xfu << 16)
#define Q8B_GPI_SPI_GO_CMD      0x1fu
#define Q8B_GPI_SPI_GO_CS       (0x7u << 8)
#define Q8B_GPI_SPI_GO_FRAG     (1u << 26)
#define Q8B_GPI_GO_RX_LEN       0x00ffffffu
#define Q8B_GPI_DMA_LEN         0x00ffffffu

/* SPI transfer commands. */
#define Q8B_GPI_SPI_CMD_TX      1u
#define Q8B_GPI_SPI_CMD_RX      2u
#define Q8B_GPI_SPI_CMD_DUPLEX  3u

/* Completion event codes (byte 11 of the 16-byte event). */
#define Q8B_GPI_EV_CODE_SUCCESS 1u
#define Q8B_GPI_EV_CODE_EOB     4u
#define Q8B_GPI_EV_CODE_EOT     4u
#define Q8B_GPI_EV_CODE_UNEXP_ERR 16u

/* Event types (byte 14). */
#define Q8B_GPI_EV_TYPE_XFER_COMPLETE 0x22u
#define Q8B_GPI_EV_TYPE_QUP_NOTIF     0x31u
#define Q8B_GPI_EV_TYPE_STALE         0xffu

typedef struct {
    /* Ring parameters; a real system provides kernel-pinned backing pages
     * and the physical addresses.  The host test validates the layout. */
    uint32_t entries;       /* power of two, per ring */
    uint32_t el_size;       /* 32 */
    uint32_t er_size;       /* 16 */
    uintptr_t desc_phys;    /* descriptor ring physical base */
    uintptr_t ev_phys;      /* event ring physical base */
} q8b_gpi_rings_t;

typedef struct {
    const struct q8b_gpi_io_s* io;
    uintptr_t base;         /* mapped GPI instance register window */
    uint32_t el_size, er_size, entries;
    uint32_t wp, rp;        /* software write/read pointers (entries) */
    uint32_t cmd_count;
    uint8_t programmed, started, allocated, poisoned, retired;
    const char* stage;
} q8b_gpi_dma_t;

/* SPI peripheral config used to build the CONFIG0 and GO TREs. */
typedef struct {
    uint8_t word_sz;        /* 0 for 8-bit SPI */
    uint8_t loopback, clock_pol_high, data_pol_high, pack_en;
    uint8_t cs;             /* CS line used by GO */
    uint8_t fragmentation;  /* keep CS asserted at end of sequence */
    uint8_t cmd;            /* Q8B_GPI_SPI_CMD_* */
    uint16_t clk_div;
    uint8_t clk_src;
    uint32_t rx_len;        /* total RX bytes for the sequence */
} q8b_gpi_spi_cfg_t;

/* All register-file accesses are funneled through this interface so the host
 * test can validate every access against a simulated GPI register file. */
typedef struct q8b_gpi_io_s {
    uint32_t (*rd)(uintptr_t base, unsigned off);
    void (*wr)(uintptr_t base, unsigned off, uint32_t value);
    void (*delay)(unsigned usec);
} q8b_gpi_io_t;

int q8b_gpi_dma_init(q8b_gpi_dma_t* dma, uintptr_t regs,
                     const q8b_gpi_rings_t* rings, const q8b_gpi_io_t* io);
int q8b_gpi_dma_program_rings(q8b_gpi_dma_t* dma);
int q8b_gpi_dma_start(q8b_gpi_dma_t* dma);
int q8b_gpi_dma_commit(q8b_gpi_dma_t* dma, uint32_t new_wp);
/* Wait for an event on a started channel.  ev_ring is the software-side copy
 * of the event ring (the caller owns the backing memory); the completion is
 * decoded from the ring slot at the current RP once the event WP advances. */
int q8b_gpi_dma_wait_event(q8b_gpi_dma_t* dma, const uint8_t* ev_ring,
                           uint64_t budget_us, uint32_t* out_code,
                           uint32_t* out_chid, uint32_t* out_length);
int q8b_gpi_dma_stop(q8b_gpi_dma_t* dma);
int q8b_gpi_dma_reset(q8b_gpi_dma_t* dma);
int q8b_gpi_dma_dealloc(q8b_gpi_dma_t* dma);

/* TRE builders (write into a caller-owned descriptor ring slot). */
int q8b_gpi_tre_config0(uint32_t* tre, const q8b_gpi_spi_cfg_t* cfg);
int q8b_gpi_tre_go(uint32_t* tre, const q8b_gpi_spi_cfg_t* cfg,
                   uint32_t rx_len);
int q8b_gpi_tre_dma(uint32_t* tre, uint32_t addr_lo, uint32_t addr_hi,
                    uint32_t len, bool rx);
int q8b_gpi_event_decode(const uint8_t* ev, uint32_t* code, uint32_t* type,
                         uint32_t* chid, uint32_t* length);

#endif
/* SPI transfer TRE sequence layer.  These are pure ring encoders: they do
 * not touch hardware.  A sequence is CONFIG0 (chain) -> GO (chain) ->
 * DMA_TX? -> DMA_RX?; the last DMA element carries IEOT (RX also BEI).
 * Callers must own the ring, run in a started channel in real use, and
 * check the return wp before committing it. */
/* Number of 32-byte TRE slots a transfer needs (2 + tx? + rx?). */
int q8b_gpi_dma_xfer_slots(const q8b_gpi_spi_cfg_t* cfg,
                           uint32_t tx_len, uint32_t rx_len);
/* Encode the transfer into ring[wp .. wp+slots); advances *out_wp by slots.
 * tx_len/rx_len may be zero for a one-direction transfer; buffers alias is
 * the caller's problem.  Returns 0 on success, -1 on invalid input/space. */
int q8b_gpi_dma_build_xfer(uint32_t* ring, uint32_t entries, uint32_t wp,
                           const q8b_gpi_spi_cfg_t* cfg,
                           uint32_t tx_lo, uint32_t tx_hi, uint32_t tx_len,
                           uint32_t rx_lo, uint32_t rx_hi, uint32_t rx_len,
                           uint32_t* out_wp);
