#include <bsp/spi.h>
#include <string.h>

#ifdef Q8B_SPI_TEST
#include "../tests/spi_test_io.h"
#else
#include <unistd.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/syscall.h>
static uint32_t hw_rd(uintptr_t base, unsigned offset) {
    uint32_t value = *(volatile uint32_t*)(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void hw_wr(uintptr_t base, unsigned offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t*)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int hw_map(uintptr_t va, uintptr_t pa, unsigned size) {
    return syscall3(SYS_MEM_MAP, va, pa, size) == va ? 0 : -1;
}
static void hw_delay(unsigned us) { usleep(us); }
static uint64_t hw_time(void) {
    uint64_t ns = 0;
    kernel_tic_nsec(&ns);
    return ns / 1000U;
}
#endif

/* Dedicated mappings: no dependency on the process's low-MMIO window. */
#define GCC_VA           0x5b000000U
#define SE_VA            0x5b100000U
#define WRAPPER_VA       0x5b104000U
#define TLMM_VA          0x58200000U
#define ROOT             0x1e868U
#define QUP_VOTE         0x52010U
#define SE6_VOTE         0x52018U
#define CLOCK_OFF        (1U << 31)

#define FORCE_DEFAULT    0x020U
#define OUTPUT_CTRL      0x024U
#define CGC_CTRL         0x028U
#define STATUS           0x040U
#define M_CLK            0x048U
#define IF_DISABLE       0x064U
#define FW_REVISION      0x068U
#define CLK_SEL          0x07cU
#define CPHA             0x224U
#define LOOPBACK         0x22cU
#define CPOL             0x230U
#define CS_INVERT        0x24cU
#define CS_SELECT        0x250U
#define BYTE_GRAN        0x254U
#define DMA_MODE         0x258U
#define TRANS_CFG        0x25cU
#define TX_PACK0         0x260U
#define TX_PACK1         0x264U
#define WORD_LEN         0x268U
#define TX_LEN           0x26cU
#define RX_LEN           0x270U
#define RX_PACK0         0x284U
#define RX_PACK1         0x288U
#define M_CMD            0x600U
#define M_CTRL           0x604U
#define M_IRQ            0x610U
#define M_IRQ_EN         0x614U
#define M_CLEAR          0x618U
#define S_CTRL           0x634U
#define S_CLEAR          0x648U
#define TX_FIFO          0x700U
#define RX_FIFO          0x780U
#define TX_STATUS        0x800U
#define RX_STATUS        0x804U
#define TX_WM            0x80cU
#define RX_WM            0x810U
#define RX_RFR           0x814U
#define DMA_TX_CLEAR     0xc44U
#define DMA_RX_CLEAR     0xd44U
/* Serial-engine DMA (not GSI/GPI): the engine walks a plain physical buffer
 * itself.  Offsets per drivers/soc/qcom/qcom-geni-se.c. */
#define DMA_TX_PTR_L     0xc30U
#define DMA_TX_PTR_H     0xc34U
#define DMA_TX_ATTR      0xc38U
#define DMA_TX_LEN       0xc3cU
#define DMA_TX_IRQ_STAT  0xc40U
#define DMA_TX_IRQ_EN_SET 0xc4cU
#define DMA_RX_PTR_L     0xd30U
#define DMA_RX_PTR_H     0xd34U
#define DMA_RX_ATTR      0xd38U
#define DMA_RX_LEN       0xd3cU
#define DMA_RX_IRQ_STAT  0xd40U
#define DMA_RX_IRQ_EN_SET 0xd4cU
#define DMA_IRQ_DONE     1U
#define DMA_IRQ_EOT      2U
#define DMA_IRQ_AHB_ERR  4U
#define GSI_EVENT        0xe18U
#define IRQ_EN           0xe1cU
#define HW_PARAM0        0xe24U
#define HW_PARAM1        0xe28U
#define DMA_CFG          0xe30U

#define CMD_DONE         1U
#define CMD_ERRORS       ((7U << 1) | (3U << 24) | (3U << 28))
#define RX_LAST          (1U << 27)
#define RX_WATERMARK     (1U << 26)
#define TX_WATERMARK     (1U << 30)
#define ACTIVE           1U

static int wait_clear(uintptr_t base, unsigned offset, uint32_t bits) {
    for (unsigned i = 0; i < 1000; i++) {
        if (!(hw_rd(base, offset) & bits))
            return 0;
        hw_delay(10);
    }
    return -1;
}

static void vote_clocks(uintptr_t base, unsigned offset, uint32_t mask) {
    uint32_t current = hw_rd(base, offset);
    if ((current & mask) != mask)
        hw_wr(base, offset, current | mask);
}

/*
 * Serial-engine descriptor table.  TW220 uses QUP2 SE6 (SPI22); gaokun3
 * uses QUP0 SE6 (spi@998000).  GCC vote/halt values come from
 * gcc-sc8280xp.c; the QUP0 root/DFS registers are not written here because
 * their addresses are unverified on that walk (firmware keeps the clock
 * on).  Pins are TLMM qup functions: TW220 83-86 f2/f1, gaokun3 154-157 f1.
 */
typedef struct {
    const char* name;
    uint32_t bus, se_pa, wrapper_pa;
    uint32_t vote, vote_se;      /* GCC vote registers (offsets) */
    uint32_t vote_mask, vote_se_mask;
    const unsigned* gates;       /* halt registers to watch before use */
    unsigned gate_count;
    uint32_t root;               /* 0 = do not touch root/DFS */
    const unsigned* pins;        /* MISO,MOSI,CLK,CS */
    const uint8_t* pin_fn;
    uint32_t max_hz;
} q8b_spi_se_t;

static const unsigned tw220_gates[] = { 0x1e004, 0x1e008, 0x1e00c, 0x1e014, 0x1e864 };
static const unsigned tw220_pins[] = { 85, 86, 83, 84 };   /* MISO,MOSI,CLK,CS */
static const uint8_t tw220_fn[]  = { 1, 1, 2, 2 };
static const unsigned gk3_gates[] = { 0x17004, 0x17008, 0x17864 };
static const unsigned gk3_pins[]  = { 154, 155, 156, 157 }; /* MISO,MOSI,CLK,CS */
static const uint8_t gk3_fn[]     = { 1, 1, 1, 1 };

static const q8b_spi_se_t SPIS[] = {
    { "tw220-spi22", 22, 0x00898000, 0x008c0000,
      0x52010, 0x52018, 0x0f, 1u << 29,
      tw220_gates, (unsigned int)(sizeof(tw220_gates) / sizeof(tw220_gates[0])),
      0x1e868, tw220_pins, tw220_fn, 19200000 },
    { "gaokun3-qup0se6", 60, 0x00998000, 0x009c0000,
      0x52008, 0x52008, (1u << 6) | (1u << 7) | (1u << 16), 1u << 16,
      gk3_gates, (unsigned int)(sizeof(gk3_gates) / sizeof(gk3_gates[0])),
      0, gk3_pins, gk3_fn, 12000000 },
};
static const q8b_spi_se_t* spi_se(unsigned bus) {
    for (unsigned i = 0; i < sizeof(SPIS) / sizeof(SPIS[0]); i++)
        if (SPIS[i].bus == bus)
            return &SPIS[i];
    return 0;
}

int q8b_spi_prepare(q8b_spi_t* spi, unsigned bus, unsigned speed_hz) {
    if (!spi)
        return -1;
    memset(spi, 0, sizeof(*spi));
    spi->stage = "arguments";
    const q8b_spi_se_t* se = spi_se(bus);
    if (!se || speed_hz == 0 || speed_hz > se->max_hz)
        return -1;
    spi->requested_hz = speed_hz;
    spi->stage = "gcc-map";
    if (hw_map(GCC_VA, 0x00100000U, 0x100000U) != 0)
        return -1;
    spi->gcc = GCC_VA;

    /* These votes are shared with other QUP users. Never reset the QUP
     * wrapper or replace a whole vote register, and never disable on exit. */
    spi->stage = "qup-clocks";
    vote_clocks(spi->gcc, se->vote, se->vote_mask);
    for (unsigned i = 0; i < se->gate_count; i++) {
        /* M/S AHB use hardware clock gating (CBCR bit 1); CLOCK_OFF can
         * describe an idle bus even with a valid vote.  Linux skips the
         * halt check in this mode; an access wakes the clock on demand. */
        bool ms_ahb = (i < 2);
        if (ms_ahb && (hw_rd(spi->gcc, se->gates[i]) & 2U))
            continue;
        if (wait_clear(spi->gcc, se->gates[i], CLOCK_OFF) != 0)
            return -1;
    }
    spi->stage = "se-clock";
    vote_clocks(spi->gcc, se->vote_se, se->vote_se_mask);
    if (se->gate_count > 0 &&
            wait_clear(spi->gcc, se->gates[se->gate_count - 1], CLOCK_OFF) != 0)
        return -1;

    spi->stage = "clock-source";
    if (!se->root) {
        /* Firmware keeps the clock on; do not write unverified root/DFS
         * registers on this SE walk.  Source stays 19.2 MHz. */
        spi->source_hz = 19200000U;
    } else if (hw_rd(spi->gcc, se->root + 0x14) & 1U) {
        /* DFS owns the root. Select an existing TCXO entry through GENI;
         * do not write CMD_RCGR/CFG_RCGR behind the DFS state machine. */
        bool found = false;
        for (unsigned level = 0; level < 8; level++) {
            uint32_t cfg = hw_rd(spi->gcc, ROOT + 0x1c + level * 4U);
            if ((cfg & 0x3700U) != 0)
                continue; /* only un-fractionalized TCXO */
            unsigned div = cfg & 31U;
            spi->source_hz = div ? 38400000U / (div + 1U) : 19200000U;
            spi->clock_index = level;
            found = true;
            break;
        }
        if (!found)
            return -1;
    } else {
        /* Non-DFS root: 19.2 MHz TCXO, divider 1, integer mode. Preserve
         * root enable and unrelated configuration bits; require UPDATE ack. */
        uint32_t cfg = hw_rd(spi->gcc, se->root + 4);
        hw_wr(spi->gcc, se->root + 4, (cfg & ~0x373fU) | 1U);
        hw_wr(spi->gcc, se->root, hw_rd(spi->gcc, se->root) | 1U);
        if (wait_clear(spi->gcc, se->root, 1U) != 0)
            return -1;
        spi->source_hz = 19200000U;
    }
    spi->stage = "clocks-ready";
    return 0;
}

/* Drop any RX words a command left behind.  A leftover word shifts the next
 * command's data by one byte pair (seen on gaokun3 as an IC ID of 83121a00
 * followed by 001a1283 on alternating reads), so every transfer ends with an
 * empty RX FIFO. */
static void flush_rx(q8b_spi_t* spi) {
    for (unsigned i = 0; i < spi->rx_depth; i++) {
        if (!(hw_rd(spi->se, RX_STATUS) & 0x1ffffffU))
            break;
        (void)hw_rd(spi->se, RX_FIFO);
    }
}

static int abort_command(q8b_spi_t* spi) {
    if (hw_rd(spi->se, STATUS) & ACTIVE) {
        hw_wr(spi->se, M_CTRL, 1U << 2); /* cancel, then bounded abort */
        if (wait_clear(spi->se, STATUS, ACTIVE) != 0) {
            hw_wr(spi->se, M_CTRL, 1U << 1);
            if (wait_clear(spi->se, STATUS, ACTIVE) != 0) {
                spi->ready = false;
                return -1;
            }
        }
    }
    hw_wr(spi->se, TX_WM, 0);
    hw_wr(spi->se, M_CLEAR, 0xffffffffU);
    return 0;
}

static void pinmux(q8b_spi_t* spi, const q8b_spi_se_t* se, bool sw_cs) {
    for (unsigned i = 0; i < 4; i++) {
        unsigned pin = se->pins[i];
        /* Software CS: leave the CS pad to the caller (plain GPIO).  The
         * engine still clocks data, but it does not own the select line. */
        if (sw_cs && i == 3)
            continue;
        unsigned off = pin * 0x1000U;
        uint32_t ctl = hw_rd(spi->tlmm, off);
        /* Clear only the function/pull/drive fields this code owns and keep
         * everything else the boot firmware programmed.  Pulls are left as
         * they are: a slave's MISO line needs its configured pull to hold a
         * defined level while no one drives it, and clearing it made the
         * gaokun3 Himax IC unreachable over GENI (register reads returned a
         * byte-shifted stream) while the bit-bang master, which sets a pull
         * explicitly, worked on the same pins. */
        ctl &= ~(15U << 2);
        ctl |= ((uint32_t)se->pin_fn[i] << 2);
        hw_wr(spi->tlmm, off, ctl);
        (void)hw_rd(spi->tlmm, off);
    }
}

int q8b_spi_open(q8b_spi_t* spi, unsigned bus, unsigned speed_hz, unsigned mode) {
    if (!spi)
        return -1;
    const q8b_spi_se_t* se = spi_se(bus);
    if (!se)
        return -1;
    if (mode & ~(15U | Q8B_SPI_SW_CS)) {
        memset(spi, 0, sizeof(*spi));
        spi->stage = "mode";
        return -1;
    }
    if (q8b_spi_prepare(spi, bus, speed_hz) != 0)
        return -1;
    spi->stage = "controller-map";
    spi->se = SE_VA;
    spi->wrapper = WRAPPER_VA;
    spi->tlmm = TLMM_VA;
    if (hw_map(spi->se, se->se_pa, 0x4000U) != 0 ||
            hw_map(spi->wrapper, se->wrapper_pa, 0x2000U) != 0 ||
            hw_map(spi->tlmm, 0x0f100000U, 0x300000U) != 0)
        return -1;
    spi->se_pa = se->se_pa;
    spi->sw_cs = (mode & Q8B_SPI_SW_CS) != 0;

    /* First access to GENI comes only after all GCC gates have acknowledged.
     * Use firmware loaded by UEFI; never load a guessed protocol image. */
    spi->stage = "firmware";
    spi->hw_version = hw_rd(spi->wrapper, 4);
    spi->firmware = hw_rd(spi->se, FW_REVISION);
    if (((spi->firmware >> 8) & 255U) != 1U)
        return -1;
    spi->stage = "fifo-interface";
    /* IF_DISABLE is a capability/configuration status, not permission to
     * rewrite protected firmware registers. On TW220 it is 1: Linux uses
     * GPI DMA in this case. Do not access 0x2000..0x2008 from this path;
     * an earlier attempted access was followed by loss of SSH, with the
     * exact faulting access/cause unconfirmed despite prepared GCC clocks. */
    if (hw_rd(spi->se, IF_DISABLE) & 1U) {
        spi->stage = "fifo-disabled-requires-gpi";
        return -1;
    }
    uint32_t param = hw_rd(spi->se, HW_PARAM0);
    spi->fifo_depth = (param >> 16) & 63U;
    spi->rx_depth = (hw_rd(spi->se, HW_PARAM1) >> 16) & 63U;
    if (spi->fifo_depth < 4 || spi->fifo_depth > 64 ||
            spi->rx_depth < 4 || spi->rx_depth > 64 ||
            ((param >> 24) & 63U) != 32U)
        return -1;
    unsigned major = (spi->hw_version >> 28) & 15U;
    unsigned minor = (spi->hw_version >> 16) & 0xfffU;
    spi->oversampling = (major == 1 && minor == 0) ? 2U : 1U;
    unsigned divisor = (spi->source_hz + speed_hz * spi->oversampling - 1U) /
            (speed_hz * spi->oversampling);
    if (divisor == 0 || divisor > 4095U)
        return -1;
    spi->speed_hz = spi->source_hz / (divisor * spi->oversampling);
    if (spi->speed_hz > se->max_hz)
        return -1;

    spi->stage = "sequencer-stop";
    if (abort_command(spi) != 0)
        return -1;
    if (hw_rd(spi->se, STATUS) & (1U << 12)) {
        hw_wr(spi->se, S_CTRL, 1U << 1);
        if (wait_clear(spi->se, STATUS, 1U << 12) != 0)
            return -1;
    }
    spi->stage = "fifo-init";
    hw_wr(spi->se, GSI_EVENT, 0);
    hw_wr(spi->se, S_CLEAR, 0xffffffffU);
    hw_wr(spi->se, DMA_TX_CLEAR, 0xffffffffU);
    hw_wr(spi->se, DMA_RX_CLEAR, 0xffffffffU);
    hw_wr(spi->se, IRQ_EN, 15U);
    hw_wr(spi->se, CGC_CTRL, hw_rd(spi->se, CGC_CTRL) | 0x7fU);
    hw_wr(spi->se, DMA_CFG, hw_rd(spi->se, DMA_CFG) | 15U);
    hw_wr(spi->se, OUTPUT_CTRL, 0x7fU);
    hw_wr(spi->se, FORCE_DEFAULT, 1);
    hw_wr(spi->se, DMA_MODE, hw_rd(spi->se, DMA_MODE) & ~1U);
    hw_wr(spi->se, M_IRQ_EN, CMD_DONE | CMD_ERRORS | (3U << 4) |
            TX_WATERMARK | RX_WATERMARK | RX_LAST);
    hw_wr(spi->se, RX_WM, spi->rx_depth - 3U);
    hw_wr(spi->se, RX_RFR, spi->rx_depth - 2U);
    /* Four bytes per FIFO word, SPI MSB-first (unlike UART's LSB-first). */
    uint32_t pack0 = (mode & Q8B_SPI_LSB_FIRST) ? 0x0004380eU : 0x0007f8feU;
    uint32_t pack1 = (mode & Q8B_SPI_LSB_FIRST) ? 0x000c3e0eU : 0x000ffefeU;
    hw_wr(spi->se, TX_PACK0, pack0);
    hw_wr(spi->se, TX_PACK1, pack1);
    hw_wr(spi->se, RX_PACK0, pack0);
    hw_wr(spi->se, RX_PACK1, pack1);
    hw_wr(spi->se, BYTE_GRAN, 0);
    hw_wr(spi->se, WORD_LEN, 4); /* 8 minus the hardware minimum of 4 */
    hw_wr(spi->se, CPHA, (mode & Q8B_SPI_CPHA) ? 1U : 0);
    hw_wr(spi->se, CPOL, (mode & Q8B_SPI_CPOL) ? 4U : 0);
    hw_wr(spi->se, LOOPBACK, (mode & Q8B_SPI_LOOPBACK) ? 1U : 0);
    /* With software CS the engine must not drive the CS pad.  Point its
     * demux at an unused chip-select channel so the pad the caller owns stays
     * under GPIO control. */
    hw_wr(spi->se, CS_SELECT, spi->sw_cs ? 3U : 0U);
    hw_wr(spi->se, CS_INVERT, 0);
    hw_wr(spi->se, TRANS_CFG, hw_rd(spi->se, TRANS_CFG) & ~2U);
    hw_wr(spi->se, CLK_SEL, spi->clock_index);
    hw_wr(spi->se, M_CLK, (divisor << 4) | 1U);
    /* No stale RX data may leak into the next transaction. */
    flush_rx(spi);
    if (hw_rd(spi->se, RX_STATUS) & 0x1ffffffU)
        return -1;
    pinmux(spi, se, (mode & Q8B_SPI_SW_CS) != 0);
    spi->ready = true;
    spi->stage = "ready";
    return 0;
}

/*
 * Move every complete RX word from the FIFO into rx.  RX_STATUS[30:28] is
 * the number of valid bytes in the LAST word (the ones before it are full),
 * and a partial last word still occupies one FIFO read.  Returns 0 or -1
 * when the engine reports more words than it can hold.
 */
static int drain_rx(q8b_spi_t* spi, uint8_t* rx, size_t len) {
    uint32_t status = hw_rd(spi->se, RX_STATUS);
    unsigned words = status & 0x1ffffffU;
    unsigned tail = 4U;

    if (words > spi->rx_depth)
        return -1;
    if ((status & (1U << 31)) && words) {
        unsigned valid = (status >> 28) & 7U;
        if (valid && valid < 4U)
            tail = valid;
    }
    while (words) {
        uint32_t value = hw_rd(spi->se, RX_FIFO);
        unsigned take = (words == 1U) ? tail : 4U;
        words--;
        if (take > len - spi->received)
            return -1;
        for (unsigned i = 0; i < take; i++)
            rx[spi->received++] = (uint8_t)(value >> (i * 8));
    }
    return 0;
}

/*
 * Read-only transfer through the serial engine's own DMA.  This is not GSI /
 * GPI DMA: the engine walks a plain physical buffer itself, so no channel, no
 * SMMU route and no descriptor ring is involved.  The caller supplies the
 * buffer's physical address (contiguous shared memory).
 *
 * Only the receive direction has a DMA path here.  A Himax record is a ~5 KiB
 * read that the CPU would otherwise copy out of the FIFO word by word, and
 * that copy is the daemon's entire CPU cost.
 *
 * Returns 0 on success.  The engine is left unusable on failure, like the
 * polled path.
 */
int q8b_spi_transfer_dma_read(q8b_spi_t* spi, void* rx, size_t len,
        uint64_t rx_pa, uint32_t timeout_us) {
    if (!spi || !spi->ready || !rx || len == 0 || len > Q8B_SPI_MAX_XFER ||
            rx_pa == 0 || (rx_pa >> 32) != 0)
        return -1;
    spi->sent = spi->received = 0;
    spi->stage = "dma-idle";
    if (hw_rd(spi->se, STATUS) & ACTIVE)
        return -1;
    hw_wr(spi->se, M_CLEAR, 0xffffffffU);
    /* Drain anything an earlier command left behind before the engine writes
     * into the buffer. */
    flush_rx(spi);
    hw_wr(spi->se, DMA_RX_IRQ_STAT, 0xffffffffU);
    hw_wr(spi->se, DMA_RX_IRQ_EN_SET, DMA_IRQ_DONE | DMA_IRQ_EOT |
            DMA_IRQ_AHB_ERR);
    hw_wr(spi->se, DMA_RX_PTR_L, (uint32_t)rx_pa);
    hw_wr(spi->se, DMA_RX_PTR_H, (uint32_t)(rx_pa >> 32));
    hw_wr(spi->se, DMA_RX_ATTR, 0);        /* RX has no EOT buffer-type bit */
    hw_wr(spi->se, DMA_RX_LEN, (uint32_t)len);
    hw_wr(spi->se, DMA_MODE, (hw_rd(spi->se, DMA_MODE) & ~1U) | 1U);
    hw_wr(spi->se, TX_LEN, (uint32_t)len);
    hw_wr(spi->se, RX_LEN, (uint32_t)len);
    hw_wr(spi->se, M_CMD, 2U << 27);       /* SPI_RX_ONLY */

    uint64_t start = hw_time();
    int ret = -1;
    spi->stage = "dma-wait";
    for (unsigned poll = 0; poll < 40000; poll++) {
        uint32_t m_irq = hw_rd(spi->se, M_IRQ);
        uint32_t d_irq = hw_rd(spi->se, DMA_RX_IRQ_STAT);
        spi->last_irq = m_irq;
        if ((m_irq & CMD_ERRORS) || (d_irq & DMA_IRQ_AHB_ERR)) {
            spi->last_status = d_irq;
            break;
        }
        if ((d_irq & DMA_IRQ_DONE) && (m_irq & CMD_DONE) &&
                wait_clear(spi->se, STATUS, ACTIVE) == 0) {
            ret = 0;
            break;
        }
        if (hw_time() - start >= timeout_us)
            break;
        /* The transfer runs without the CPU: poll coarsely and sleep in
         * between, which is the point of using DMA at all. */
        hw_delay(50);
    }
    hw_wr(spi->se, DMA_MODE, hw_rd(spi->se, DMA_MODE) & ~1U);
    hw_wr(spi->se, DMA_RX_IRQ_STAT, 0xffffffffU);
    if (ret == 0) {
        hw_wr(spi->se, M_CLEAR, 0xffffffffU);
        spi->received = len;
        spi->stage = "ready";
        return 0;
    }
    (void)abort_command(spi);
    spi->ready = false;
    spi->stage = "dma-failed";
    return -1;
}

int q8b_spi_transfer(q8b_spi_t* spi, const void* tx_data, void* rx_data, size_t len) {
    const uint8_t* tx = tx_data;
    uint8_t* rx = rx_data;
    if (!spi || !spi->ready || (!tx && !rx) || len == 0 || len > Q8B_SPI_MAX_XFER)
        return -1;
    spi->sent = spi->received = 0;
    spi->stage = "transfer-idle";
    if (hw_rd(spi->se, STATUS) & ACTIVE)
        return -1;
    hw_wr(spi->se, M_CLEAR, 0xffffffffU);
    /* A previous command may have left RX words in the FIFO (error paths,
     * cancelled commands).  Starting with a clean FIFO is what makes the
     * first word of this transfer our data and not the last word of the
     * previous one. */
    flush_rx(spi);
    hw_wr(spi->se, TX_WM, 0);
    /* An RX-only command still has to clock len bytes, so TX_LEN is
     * set for both directions; the TX FIFO is fed 0xff in that case
     * (see the filler branch below). */
    hw_wr(spi->se, TX_LEN, len);
    hw_wr(spi->se, RX_LEN, rx ? len : 0);
    /* FIFO full-duplex is opcode 1|2, NOT the GSI opcode named SPI_TX_RX=7. */
    unsigned opcode = (tx ? 1U : 0) | (rx ? 2U : 0);
    hw_wr(spi->se, M_CMD, opcode << 27);
    uint64_t start = hw_time();
    uint64_t budget = ((uint64_t)len * 8000000U / spi->speed_hz) + 20000U;
    spi->stage = "transfer";
    unsigned idle_polls = 0;
    for (unsigned poll = 0; poll < 40000; poll++) {
        uint32_t irq = hw_rd(spi->se, M_IRQ);
        spi->last_irq = irq;
#ifdef Q8B_SPI_TRACE
        if (poll < 24)
            printf("spi_trace[%u] irq=%08x txs=%08x rxs=%08x st=%08x sent=%u recv=%u\n",
                    poll, irq, hw_rd(spi->se, TX_STATUS), hw_rd(spi->se, RX_STATUS),
                    hw_rd(spi->se, STATUS), (unsigned)spi->sent, (unsigned)spi->received);
#endif
        if (irq & CMD_ERRORS)
            break;
        if (tx && spi->sent < len) {
            unsigned used = hw_rd(spi->se, TX_STATUS) & 0xfffffffU;
            if (used > spi->fifo_depth)
                break;
            unsigned room = spi->fifo_depth - used;
            while (room-- && spi->sent < len) {
                uint32_t value = 0;
                for (unsigned i = 0; i < 4 && spi->sent < len; i++)
                    value |= (uint32_t)tx[spi->sent++] << (i * 8);
                hw_wr(spi->se, TX_FIFO, value);
            }
        } else if (spi->sent < len) {
            /* RX-only: the SPI engine clocks TX_LEN bytes and produces one
             * RX word for every word it shifts, so the TX FIFO must be fed
             * for the whole length even when the caller supplies no TX data
             * (the Linux geni-spi driver drives the buffer for the same
             * reason).  0xff is the idle MOSI level in mode 3. */
            unsigned used = hw_rd(spi->se, TX_STATUS) & 0xfffffffU;
            if (used > spi->fifo_depth)
                break;
            unsigned room = spi->fifo_depth - used;
            while (room-- && spi->sent < len) {
                uint32_t value = 0;
                for (unsigned i = 0; i < 4 && spi->sent < len; i++)
                    value |= 0xffU << (i * 8);
                hw_wr(spi->se, TX_FIFO, value);
                spi->sent++;
            }
        }
        /* Drain after filling TX so aliased TX/RX buffers remain valid. */
        if (rx) {
            int drained = drain_rx(spi, rx, len);
            if (drained < 0)
                break;
        }
        hw_wr(spi->se, M_CLEAR, irq);
        if (irq & CMD_DONE) {
            /* CMD_DONE says the last byte was clocked, not that its RX word
             * has landed in the FIFO (Linux handles both in one ISR for the
             * same reason).  Drain once more before deciding, or the last
             * word of every frame is lost. */
            /* The RX FIFO entry for the last clocked byte can land a few
             * cycles after CMD_DONE (Linux drains in the same ISR that
             * reports it and tolerates a short wait).  Poll the FIFO for a
             * bounded window before declaring the transfer short. */
            if (rx && spi->received < len) {
                for (unsigned settle = 0; settle < 200; settle++) {
                    if (hw_rd(spi->se, RX_STATUS) & 0x1ffffffU)
                        break;
                    hw_delay(1);
                }
            }
            if (rx) {
                int drained = drain_rx(spi, rx, len);
                if (drained < 0)
                    break;
            }
            if ((!tx || spi->sent == len) && (!rx || spi->received == len) &&
                    wait_clear(spi->se, STATUS, ACTIVE) == 0) {
                flush_rx(spi);
                spi->stage = "ready";
                return 0;
            }
            break;
        }
        /* Checking the clock is not free, and the FIFO needs attention every
         * few microseconds at 9.6 MHz: look at the deadline every 16 polls
         * instead of one, and sleep a little longer between polls so the loop
         * is not a pure spin.  Measured on the gaokun3 record read this keeps
         * the transfer at the same speed while cutting the CPU the poll loop
         * costs. */
        if ((++idle_polls & 15U) == 0U && hw_time() - start >= budget)
            break;
        hw_delay(4);
    }
    spi->last_status = hw_rd(spi->se, STATUS);
    (void)abort_command(spi);
    /* A failed command can leave RX words behind; the next command would
     * then return those stale words as its own data. */
    flush_rx(spi);
    /* Stop after any error: no other client may continue a partial command. */
    spi->ready = false;
    spi->stage = "transfer-failed";
    return -1;
}
