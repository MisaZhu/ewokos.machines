#include <dev/uart.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Kernel console on the SC8280XP GENI debug UART (uart17, 0x884000).
 *
 * UEFI already brings up the QUP clock, the TLMM pinmux and the baud rate;
 * the firmware leaves the SE in UART mode.  We only normalise the packet
 * format and TX path, mirroring what Linux's qcom_geni earlycon does.  In
 * particular we do NOT touch the clock rate: the UART is shared with the
 * firmware console until ExitBootServices.
 */

#ifdef Q8B_QEMU_TEST
extern void q8b_dbg_putc(char c);

int32_t uart_dev_init(uint32_t baud) {
    (void)baud;
    return 0;
}

int32_t uart_write(const void* data, uint32_t size) {
    const char* p = (const char*)data;
    for (uint32_t i = 0; i < size; i++) {
        if (p[i] == '\n')
            q8b_dbg_putc('\r');
        q8b_dbg_putc(p[i]);
    }
    return (int32_t)size;
}

#else

#define UART_BASE   (MMIO_BASE + Q8B_UART17_OFF)

#define SE_GENI_TX_PACKING_CFG0   0x260
#define SE_GENI_TX_PACKING_CFG1   0x264
#define SE_GENI_RX_PACKING_CFG0   0x284
#define SE_GENI_RX_PACKING_CFG1   0x288
#define SE_UART_TX_TRANS_CFG      0x25c
#define SE_UART_TX_WORD_LEN       0x268
#define SE_UART_TX_STOP_BIT_LEN   0x26c
#define SE_UART_RX_TRANS_CFG      0x280
#define SE_UART_RX_WORD_LEN       0x28c
#define SE_UART_RX_STALE_CNT      0x294
#define SE_UART_TX_PARITY_CFG     0x2a4
#define SE_UART_RX_PARITY_CFG     0x2a8
#define SE_GENI_TX_WATERMARK_REG  0x80c
#define SE_GENI_M_IRQ_CLEAR       0x618

#define UART_PACKING_CFG0         0x0F
#define UART_PACKING_CFG1         0x00
#define UART_CTS_MASK             (1u << 1)
#define DEF_TX_WM                 2
#define M_CMD_DONE_EN             (1u << 0)
#define M_CMD_ABORT_EN            (1u << 5)
#define M_TX_FIFO_WATERMARK_EN    (1u << 30)

#define REG(off) (UART_BASE + (off))

extern void q8b_dbg_putc(char c);
extern void q8b_dbg_puts(const char* s);
extern void q8b_dbg_write(const char* s, uint32_t size);
extern void q8b_dbg_dec(uint64_t v);

int32_t uart_dev_init(uint32_t baud) {
    /* UEFI BIOS already programmed UART17 (QUP2/SE1, clock, pinmux, baud).
     * Do not touch any UART register here; only TX in q8b_dbg_putc(). */
    (void)baud;
    return 0;
}

int32_t uart_write(const void* data, uint32_t size) {
    q8b_dbg_write((const char*)data, size);
    return (int32_t)size;
}

#endif /* Q8B_QEMU_TEST */

/* ------------------------------------------------------------------
 * Early QUP2/SE1 UART17 bring-up for Q8B.
 *
 * UEFI programs the UART, but its serial driver is stopped at
 * ExitBootServices, which gates the QUP2/SE1 clocks and may leave the
 * 40-pin pins without their peripheral mux.  Re-enable the SC8280XP GCC
 * branches, restore GPIO63/64 to QUP17, and re-init the GENI SE exactly
 * like Linux's qcom_geni_serial + qcom-geni-se do.  The framebuffer markers
 * below make progress visible even when the physical UART is unavailable.
 * ------------------------------------------------------------------ */
#ifdef Q8B_BOARD
#define Q8B_EARLY_TEXT __attribute__((section(".text.q8b_early"), noinline))
#define Q8B_GCC_BASE              0x00100000u
#define Q8B_GCC_QUP2_ENABLE       (Q8B_GCC_BASE + 0x52010u)
#define Q8B_GCC_QUP2_S_AHB_HALT   (Q8B_GCC_BASE + 0x1e008u)
#define Q8B_GCC_QUP2_M_AHB_HALT   (Q8B_GCC_BASE + 0x1e004u)
#define Q8B_GCC_QUP2_CORE_HALT    (Q8B_GCC_BASE + 0x1e00cu)
#define Q8B_GCC_QUP2_CORE2X_HALT  (Q8B_GCC_BASE + 0x1e014u)
#define Q8B_GCC_QUP2_S1_HALT      (Q8B_GCC_BASE + 0x1e274u)
#define Q8B_TLMM_BASE             0x0f100000u
#define Q8B_TLMM_GPIO63_CTL       (Q8B_TLMM_BASE + 0x3f000u)
#define Q8B_TLMM_GPIO64_CTL       (Q8B_TLMM_BASE + 0x40000u)
#define Q8B_UART17_BASE           0x00884000u

#define Q8B_SE_IRQ_EN             0xe1cu
#define Q8B_SE_GENI_DMA_MODE_EN   0x258u
#define Q8B_SE_GSI_EVENT_EN       0xe18u
#define Q8B_SE_GENI_CGC_CTRL      0x28u
#define Q8B_SE_GENI_STATUS        0x40u
#define Q8B_SE_GENI_SER_M_CLK     0x48u
#define Q8B_SE_GENI_SER_S_CLK     0x4cu
#define Q8B_SE_GENI_CLK_SEL       0x7cu
#define Q8B_SE_GENI_BYTE_GRAN     0x254u
#define Q8B_SE_DMA_GENERAL_CFG    0xe30u
#define Q8B_SE_GENI_M_IRQ_CLEAR   0x618u
#define Q8B_SE_GENI_M_IRQ_EN      0x614u
#define Q8B_SE_GENI_M_CMD_CTRL    0x604u
#define Q8B_SE_GENI_S_CMD_CTRL    0x634u
#define Q8B_SE_GENI_S_IRQ_STATUS  0x640u
#define Q8B_SE_GENI_S_IRQ_EN      0x644u
#define Q8B_SE_GENI_S_IRQ_CLEAR   0x648u
#define Q8B_SE_DMA_TX_IRQ_CLEAR   0xc50u
#define Q8B_SE_DMA_RX_IRQ_CLEAR   0xd44u
#define Q8B_SE_IRQ_EN              0xe1cu
#define Q8B_SE_GENI_TX_WATERMARK  0x80cu
#define Q8B_SE_GENI_RX_WATERMARK  0x810u
#define Q8B_SE_GENI_RX_RFR_WATERMARK 0x814u
#define Q8B_SE_UART_TX_TRANS_CFG  0x25cu
#define Q8B_SE_UART_TX_WORD_LEN   0x268u
#define Q8B_SE_UART_TX_STOP_BIT   0x26cu
#define Q8B_SE_UART_TX_PARITY     0x2a4u
#define Q8B_SE_UART_TX_PACK0      0x260u
#define Q8B_SE_UART_TX_PACK1      0x264u
#define Q8B_SE_GENI_M_CMD0        0x600u
#define Q8B_SE_GENI_M_IRQ_STATUS  0x610u
#define Q8B_SE_GENI_TX_FIFO       0x700u

/* APSS watchdog from sc8280xp.dtsi.  Keep this in the early text section so
 * the boot entry can stop a firmware-armed timer before enabling the MMU. */
#define Q8B_APSS_WDT_BASE         0x17c10000ULL
#define Q8B_APSS_WDT_RST          0x04u
#define Q8B_APSS_WDT_EN           0x08u

static inline uint32_t q8b_rd(uint64_t base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void q8b_wr(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static Q8B_EARLY_TEXT int q8b_poll_set(uint64_t base, uint32_t off, uint32_t mask, int set) {
    for (volatile uint32_t i = 0; i < 200000; i++) {
        if (((q8b_rd(base, off) & mask) != 0) == (set != 0))
            return 0;
    }
    return -1;
}

static Q8B_EARLY_TEXT void q8b_geni_io_init(uint64_t base) {
    uint32_t v = q8b_rd(base, Q8B_SE_GENI_CGC_CTRL);
    v |= 0x7fu;                    /* DEFAULT_CGC_EN: GENMASK(6,0) */
    q8b_wr(base, Q8B_SE_GENI_CGC_CTRL, v);

    v = q8b_rd(base, Q8B_SE_DMA_GENERAL_CFG);
    v |= 0xfu;                     /* AHB_SEC + DMA AHB/TX/RX CGC */
    q8b_wr(base, Q8B_SE_DMA_GENERAL_CFG, v);

    q8b_wr(base, 0x24u, 0x7fu);    /* DEFAULT_IO_OUTPUT_CTRL_MSK */
    q8b_wr(base, 0x20u, 0x1u);     /* GENI_FORCE_DEFAULT_REG */
}

static Q8B_EARLY_TEXT void q8b_geni_io_set_mode(uint64_t base) {
    uint32_t v = q8b_rd(base, Q8B_SE_IRQ_EN);
    v |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
    q8b_wr(base, Q8B_SE_IRQ_EN, v);

    v = q8b_rd(base, Q8B_SE_GENI_DMA_MODE_EN);
    v &= ~1u;
    q8b_wr(base, Q8B_SE_GENI_DMA_MODE_EN, v);
    q8b_wr(base, Q8B_SE_GSI_EVENT_EN, 0);
}

static Q8B_EARLY_TEXT void q8b_geni_reset_transfer(uint64_t base) {
    uint32_t status = q8b_rd(base, Q8B_SE_GENI_STATUS);

    /* UEFI may leave either sequencer active when its driver is stopped. */
    if (status & (1u << 0)) {
        q8b_wr(base, Q8B_SE_GENI_M_CMD_CTRL, 1u << 1);
        q8b_poll_set(base, Q8B_SE_GENI_STATUS, 1u << 0, 0);
    }
    if (status & (1u << 12)) {
        q8b_wr(base, Q8B_SE_GENI_S_CMD_CTRL, 1u << 1);
        q8b_poll_set(base, Q8B_SE_GENI_STATUS, 1u << 12, 0);
    }

    /* Match geni_se_irq_clear()/geni_se_init() for a UART SE.  In
     * particular, clear DMA and wrapper IRQ state left by UEFI before
     * selecting FIFO mode. */
    q8b_wr(base, Q8B_SE_GSI_EVENT_EN, 0);
    q8b_wr(base, Q8B_SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    q8b_wr(base, Q8B_SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
    q8b_wr(base, Q8B_SE_DMA_TX_IRQ_CLEAR, 0xffffffffu);
    q8b_wr(base, Q8B_SE_DMA_RX_IRQ_CLEAR, 0xffffffffu);
    q8b_wr(base, Q8B_SE_IRQ_EN, 0xffffffffu);
    q8b_wr(base, Q8B_SE_GENI_M_IRQ_EN, 0x33c0007fu);
    q8b_wr(base, Q8B_SE_GENI_S_IRQ_EN, 0x03003e3fu);
    q8b_wr(base, Q8B_SE_GENI_RX_WATERMARK, 2u);
    q8b_wr(base, Q8B_SE_GENI_RX_RFR_WATERMARK, 30u);
    q8b_wr(base, Q8B_SE_GENI_BYTE_GRAN, 0u); /* 4 x 8-bit packing */

    /* Preserve the firmware-selected divider/source, but re-enable both
     * serializer clocks if ExitBootServices left their enable bit cleared. */
    uint32_t clk = q8b_rd(base, Q8B_SE_GENI_SER_M_CLK);
    clk |= 1u;
    q8b_wr(base, Q8B_SE_GENI_SER_M_CLK, clk);
    q8b_wr(base, Q8B_SE_GENI_SER_S_CLK, clk);
}

Q8B_EARLY_TEXT void q8b_early_watchdog_disable(void) {
    volatile uint32_t* wdt = (volatile uint32_t*)(uintptr_t)Q8B_APSS_WDT_BASE;

    /* qcom,kpss-wdt stop sequence: clear EN, then reset the counter. */
    wdt[Q8B_APSS_WDT_EN / 4u] = 0;
    wdt[Q8B_APSS_WDT_RST / 4u] = 1;
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

static Q8B_EARLY_TEXT void q8b_geni_packing(uint64_t base) {
    /* bpw=8, pack_words=4, msb_to_lsb=false; equivalent to
     * geni_se_config_packing(se, 8, 4, false, true, true). */
    q8b_wr(base, Q8B_SE_UART_TX_PACK0, 0x0004380eu);
    q8b_wr(base, Q8B_SE_UART_TX_PACK1, 0x000c3e0eu);
    q8b_wr(base, 0x284u, 0x0004380eu);
    q8b_wr(base, 0x288u, 0x000c3e0eu);
}

static Q8B_EARLY_TEXT void q8b_uart17_pinmux(void) {
    /* SC8280XP GPIO63/64 are the board's UART17 TX/RX pins (Pin 8/10).
     * PINGROUP() places mux bits at [4:2], with QUP17 as function 1. */
    volatile uint32_t* tx = (volatile uint32_t*)(uintptr_t)Q8B_TLMM_GPIO63_CTL;
    volatile uint32_t* rx = (volatile uint32_t*)(uintptr_t)Q8B_TLMM_GPIO64_CTL;
    *tx = (*tx & ~(7u << 2)) | (1u << 2);
    *rx = (*rx & ~(7u << 2)) | (1u << 2);
}

Q8B_EARLY_TEXT void q8b_early_uart_init(void) {
    volatile uint32_t* gcc = (volatile uint32_t*)(uintptr_t)Q8B_GCC_QUP2_ENABLE;
    *gcc |= (1u << 0) | (1u << 1) | (1u << 2) |
            (1u << 3) | (1u << 5);

    q8b_poll_set(Q8B_GCC_BASE, Q8B_GCC_QUP2_CORE_HALT - Q8B_GCC_BASE, 1u, 0);
    q8b_poll_set(Q8B_GCC_BASE, Q8B_GCC_QUP2_CORE2X_HALT - Q8B_GCC_BASE, 1u, 0);
    q8b_poll_set(Q8B_GCC_BASE, Q8B_GCC_QUP2_S_AHB_HALT - Q8B_GCC_BASE, 1u, 0);
    q8b_poll_set(Q8B_GCC_BASE, Q8B_GCC_QUP2_M_AHB_HALT - Q8B_GCC_BASE, 1u, 0);
    q8b_poll_set(Q8B_GCC_BASE, Q8B_GCC_QUP2_S1_HALT - Q8B_GCC_BASE, 1u, 0);
    q8b_uart17_pinmux();
    q8b_geni_reset_transfer(Q8B_UART17_BASE);
    q8b_geni_io_init(Q8B_UART17_BASE);
    q8b_geni_io_set_mode(Q8B_UART17_BASE);

    q8b_geni_packing(Q8B_UART17_BASE);

    /* UART_CTS_MASK means ignore the CTS input in GENI's UART protocol. */
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_UART_TX_TRANS_CFG, 0x2u);
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_UART_TX_PARITY, 0);
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_UART_TX_WORD_LEN, 8);
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_UART_TX_STOP_BIT, 0);
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_GENI_TX_WATERMARK, 2);
    q8b_wr(Q8B_UART17_BASE, Q8B_SE_GENI_M_IRQ_CLEAR,
           (1u << 0) | (1u << 5) | (1u << 30));

}
#endif
