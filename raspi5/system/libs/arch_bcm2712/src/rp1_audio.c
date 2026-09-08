/*
 * RP1 audio_out + DW AXI DMAC playback backend for BCM2712 (Raspberry Pi 5).
 *
 * See rp1_audio.h for the caller-facing model. Everything here is polled: no
 * RP1 interrupt reaches user space, so the DMA channel is left running around
 * a closed descriptor ring and its position is read back from CH_LLP.
 *
 * Register maps and programming sequences come from:
 *   sound/soc/bcm/rp1_aout.c                 audio_out block and its tuning
 *   drivers/clk/clk-rp1.c                    PLL_AUDIO_CORE/SEC, CLK_AUDIO_OUT
 *   drivers/dma/dw-axi-dmac-platform.c       DMAC, RPi tree (use_cfg2 branch)
 *   arch/arm64/boot/dts/broadcom/rp1.dtsi    addresses, snps,* dma properties
 *   drivers/pinctrl/pinctrl-rp1.c            "aaud" is funcsel 4 on GPIO12/13
 */
#include <arch/bcm2712/rp1_audio.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <string.h>
#include <unistd.h>

#define aout_dmb() __asm__ volatile("dmb sy" ::: "memory")

#if PI5_RP1_WIN_OFF != 0x06000000
#error "RP1 window must match the raspi5 kernel mapping"
#endif

/* ---------------------------------------------------------------- windows */

/* offsets inside the RP1 register file (rp1.dtsi) */
#define RP1_CLK_REG_OFF   0x00018000u   /* clocks@18000, 0x10038 bytes */
#define RP1_AOUT_REG_OFF  0x00094000u   /* audio_out@94000, 0x4000 bytes */
#define RP1_DMAC_REG_OFF  0x00188000u   /* dma@188000, 0x1000 bytes */

/* the same offsets as seen from _mmio_base */
#define RP1_CLK_OFF   (PI5_RP1_WIN_OFF + RP1_CLK_REG_OFF)
#define RP1_AOUT_OFF  (PI5_RP1_WIN_OFF + RP1_AOUT_REG_OFF)
#define RP1_DMAC_OFF  (PI5_RP1_WIN_OFF + RP1_DMAC_REG_OFF)

/*
 * Bus addresses as the RP1 DMAC sees them.
 *
 * Host RAM: bcm2712.dtsi's pcie2 dma-ranges put all of DRAM at
 * +0x10_0000_0000, the same offset xhci.c uses for its rings.
 *
 * RP1 registers: rp1.dtsi declares
 *   ranges = <0xc0 0x40000000  0x02000000 0x00 0x00000000  0x00 0x00410000>
 * i.e. the register file is also aliased at 0xC0_4000_0000 + offset. That is
 * what rp1_aout.c ends up handing to dmaengine (physaddr + SAMPLE_FIFO), and
 * what Circle's VIRT_TO_RP1_BUS() builds.
 */
#define RP1_RAM_BUS_OFF      0x1000000000ULL
#define RP1_PERIPH_BUS_BASE  0xC040000000ULL
#define RP1_PERIPH_BUS_MASK  0x3FFFFFFFULL
#define RP1_PERIPH_BUS(off)  (RP1_PERIPH_BUS_BASE | ((off) & RP1_PERIPH_BUS_MASK))

/* ----------------------------------------------------------------- clocks */

/* clk-rp1.c register offsets, relative to the clocks bank base */
#define PLL_AUDIO_CS          0x0c000
#define PLL_AUDIO_PWR         0x0c004
#define PLL_AUDIO_FBDIV_INT   0x0c008
#define PLL_AUDIO_FBDIV_FRAC  0x0c00c
#define PLL_AUDIO_SEC         0x0c014

#define PLL_CS_LOCK           (1u << 31)
#define PLL_CS_REFDIV_SHIFT   0
#define PLL_PWR_DSMPD         (1u << 2)
#define PLL_PWR_MASK          0x3fu
#define PLL_SEC_RST           (1u << 16)
#define PLL_SEC_IMPL          (1u << 31)
#define PLL_SEC_DIV_SHIFT     8
#define PLL_SEC_DIV_MASK      0x00001f00u

#define CLK_DMA_CTRL          0x00044
#define CLK_DMA_DIV_INT       0x00048
#define CLK_AUDIO_OUT_CTRL    0x000a4
#define CLK_AUDIO_OUT_DIV_INT 0x000a8

#define CLK_CTRL_ENABLE       (1u << 11)
#define CLK_CTRL_SRC_SHIFT    0
#define CLK_CTRL_AUXSRC_SHIFT 5
#define CLK_CTRL_AUXSRC_MASK  0x000003e0u

/*
 * PLL_AUDIO_CORE runs at 1536 MHz off the 50 MHz xosc (rp1.dtsi:
 * assigned-clock-rates = <1536000000>, and "Must match the XOSC frequency"
 * on the 50 MHz RP1_CLK_SLOW_SYS entry).
 *
 * get_pll_core_divider(1536 MHz, 50 MHz) gives fbdiv_int 30 and
 * fbdiv_frac 12079596 (= round(0.72 * 2^24)); its own calc_rate comes back
 * out at exactly 1536000000, so the constants are used here instead of
 * redoing the 64-bit division.
 */
#define PLL_AUDIO_FBDIV_INT_VAL   30u
#define PLL_AUDIO_FBDIV_FRAC_VAL  12079596u

/*
 * rp1_pll_divider_set_rate(): div = clamp(DIV_ROUND_UP(1536/153.6), 8, 19)
 * = 10, giving PLL_AUDIO_SEC = 153.6 MHz.
 */
#define PLL_AUDIO_SEC_DIV_VAL     10u

/*
 * rp1_clock_choose_div(153.6 MHz, 153.6 MHz) = 1. The parent list of
 * RP1_CLK_AUDIO_OUT is {"", pll_audio_sec, pll_video_sec, xosc, gp0..gp5}
 * with num_std_parents = 0, so pll_audio_sec is aux index 1.
 * clk_src_mask is 0 for this clock in clk-rp1.c, which makes the SRC field
 * write in rp1_clock_set_parent() a no-op; only AUXSRC is programmed here.
 */
#define CLK_AUDIO_OUT_DIV_VAL         1u
#define CLK_AUDIO_OUT_AUXSRC_PLL_SEC  1u
/* RP1_CLK_DMA parent list is {pll_sys_pri_ph, pll_video, xosc, gp0..gp5} */
#define CLK_DMA_AUXSRC_XOSC           2u

/* clk-rp1.c LOCK_TIMEOUT_NS is 100 ms */
#define PLL_LOCK_POLL_MAX  50u
#define PLL_LOCK_POLL_US   2000u

/* -------------------------------------------------------------- audio_out */

#define AUDIO_OUT_CTRL                  0x0000
#define AUDIO_OUT_CTRL_PERIPH_EN        (1u << 31)
#define AUDIO_OUT_CTRL_CIC_RATE_MASK    0x00000f00u
#define AUDIO_OUT_CTRL_CIC_RATE_SHIFT   8
#define AUDIO_OUT_CTRL_CHANNEL_SWAP     (1u << 2)
#define AUDIO_OUT_CTRL_RIGHT_CH_ENABLE  (1u << 1)
#define AUDIO_OUT_CTRL_LEFT_CH_ENABLE   (1u << 0)

#define AUDIO_OUT_SDMCTL_LEFT           0x0004
#define AUDIO_OUT_SDMCTL_RIGHT          0x0008
#define AUDIO_OUT_SDMCTL_BIAS_MASK      0xffff0000u
#define AUDIO_OUT_SDMCTL_BIAS_SHIFT     16
#define AUDIO_OUT_SDMCTL_CLAMP_EN       (1u << 5)
#define AUDIO_OUT_SDMCTL_DITHER_EN      (1u << 4)
#define AUDIO_OUT_SDMCTL_BITWIDTH_MASK  0x0000000fu
#define AUDIO_OUT_SDMCTL_BITWIDTH_SHIFT 0

#define AUDIO_OUT_QCLAMP_LEFT           0x000c
#define AUDIO_OUT_QCLAMP_RIGHT          0x0010
#define AUDIO_OUT_QCLAMP_MAX_MASK       0xffff0000u
#define AUDIO_OUT_QCLAMP_MAX_SHIFT      16
#define AUDIO_OUT_QCLAMP_MIN_MASK       0x0000ffffu
#define AUDIO_OUT_QCLAMP_MIN_SHIFT      0

#define AUDIO_OUT_MUTE_CTRL_LEFT        0x0014
#define AUDIO_OUT_MUTE_CTRL_RIGHT       0x0018
#define AUDIO_OUT_MUTE_PERIOD_MASK      0x00ff0000u
#define AUDIO_OUT_MUTE_PERIOD_SHIFT     16
#define AUDIO_OUT_MUTE_STEP_MASK        0x0000ff00u
#define AUDIO_OUT_MUTE_STEP_SHIFT       8
#define AUDIO_OUT_MUTE_INIT_UNMUTE      (1u << 5)
#define AUDIO_OUT_MUTE_INIT_MUTE        (1u << 4)
/* transitional FSM states, bits 0 and 2 of MUTE_CTRL */
#define AUDIO_OUT_MUTE_FSM_BUSY_MASK    0x00000005u

#define AUDIO_OUT_PWMCTL_LEFT           0x001c
#define AUDIO_OUT_PWMRANGE_LEFT         0x0020
#define AUDIO_OUT_PWMCTL_RIGHT          0x0028
#define AUDIO_OUT_PWMRANGE_RIGHT        0x002c

#define AUDIO_OUT_FIFO_CONTROL          0x0034
#define AUDIO_OUT_FIFO_DMA_DREQ_EN      (1u << 31)
#define AUDIO_OUT_FIFO_FLUSH_DONE       (1u << 25)
#define AUDIO_OUT_FIFO_FLUSH            (1u << 24)
#define AUDIO_OUT_FIFO_DWELL_MASK       0x001f0000u
#define AUDIO_OUT_FIFO_DWELL_SHIFT      16
#define AUDIO_OUT_FIFO_THRESHOLD_MASK   0x0000003fu
#define AUDIO_OUT_FIFO_THRESHOLD_SHIFT  0

#define AUDIO_OUT_SAMPLE_FIFO           0x0038

#define FIELD(v, mask, shift)  (((uint32_t)(v) << (shift)) & (mask))

/* audio_mute_sync() allows 500 * 1..5 ms; the ramp itself is ~300 ms */
#define MUTE_SYNC_POLL_MAX  400u
#define MUTE_SYNC_POLL_US   1000u

/* GPIO12/13 carry audio_out (uConsole CM5 carrier: AUD_PWM0 / AUD_PWM1) */
#define RP1_AUDIO_GPIO_LEFT   12u
#define RP1_AUDIO_GPIO_RIGHT  13u

/* ------------------------------------------------------------------- DMAC */

#define DMAC_COMMON_REG_LEN   0x100u
#define DMAC_CHAN_REG_LEN     0x100u
#define DMAC_NUM_CHANNELS     8u

#define DMAC_CFG              0x010
#define DMAC_CHEN             0x018
#define DMAC_RESET            0x058

#define DMAC_EN_MASK          (1u << 0)
#define DMAC_INT_EN_MASK      (1u << 1)
#define DMAC_CHAN_EN_SHIFT    0
#define DMAC_CHAN_EN_WE_SHIFT 8

#define CH_SAR             0x000
#define CH_DAR             0x008
#define CH_BLOCK_TS        0x010
#define CH_CTL_L           0x018
#define CH_CTL_H           0x01c
#define CH_CFG_L           0x020
#define CH_CFG_H           0x024
#define CH_LLP_L           0x028
#define CH_LLP_H           0x02c
#define CH_STATUS          0x030
#define CH_INTSTATUS_ENA   0x080
#define CH_INTSTATUS       0x088
#define CH_INTSIGNAL_ENA   0x090
#define CH_INTCLEAR        0x098

#define CH_CTL_L_DST_MSIZE_POS 18
#define CH_CTL_L_SRC_MSIZE_POS 14
#define CH_CTL_L_DST_WIDTH_POS 11
#define CH_CTL_L_SRC_WIDTH_POS 8
#define CH_CTL_L_DST_INC_POS   6
#define CH_CTL_L_SRC_INC_POS   4
#define CH_CTL_L_INC           0u
#define CH_CTL_L_NOINC         1u

#define CH_CTL_H_ARLEN_EN    (1u << 6)
#define CH_CTL_H_ARLEN_POS   7
#define CH_CTL_H_AWLEN_EN    (1u << 15)
#define CH_CTL_H_AWLEN_POS   16
#define CH_CTL_H_LLI_LAST    (1u << 30)
#define CH_CTL_H_LLI_VALID   (1u << 31)

/*
 * CH_CFG uses the "CFG2" field layout on RP1: the linux driver switches to
 * it when snps,dma-targets exceeds 16, and rp1.dtsi declares 64. Circle
 * hard-codes the same layout.
 */
#define CH_CFG_L_SRC_MULTBLK_POS  0
#define CH_CFG_L_DST_MULTBLK_POS  2
#define CH_CFG2_L_SRC_PER_POS     4
#define CH_CFG2_L_DST_PER_POS     11
#define CH_CFG2_H_TT_FC_POS       0
#define CH_CFG2_H_HS_SEL_SRC_POS  3
#define CH_CFG2_H_HS_SEL_DST_POS  4
#define CH_CFG2_H_PRIORITY_POS    20

#define MBLK_TYPE_LL             3u
#define HS_SEL_HW                0u
#define TT_FC_MEM_TO_PER_DMAC    1u

#define BURST_TRANS_LEN_1        0u
#define BURST_TRANS_LEN_4        1u
#define BURST_TRANS_LEN_16       3u
/* AXI ARLEN/AWLEN carry beats-1, so an 8-beat cap is the field value 7 */
#define ARWLEN_8                 7u

/* dt-bindings/mfd/rp1.h */
#define RP1_DMA_AUDIO_OUT        29u

/*
 * Channel 0: rp1.dtsi gives snps,priority = <0 1 2 3 4 5 6 7> and
 * snps,axi-max-burst-len = <8 8 4 4 4 4 4 4>, and nothing else in EwokOS
 * uses the RP1 DMAC (i2c and spi are polled).
 */
#define AUDIO_DMA_CHANNEL        0u
#define AUDIO_DMA_PRIORITY       0u

/* DMAC_RESET clears within a few core clocks; linux allows 1000 spins */
#define DMAC_RESET_POLL_MAX      100000u
/* Circle polls up to 50 ms for the enable bit to drop after a disable */
#define DMAC_STOP_POLL_MAX       50u
#define DMAC_STOP_POLL_US        1000u

/* the LLI must be naturally aligned; dma_alloc only guarantees a page */
#define RP1_LLI_ALIGN            64u

/* dw-axi-dmac's linked list item, exactly 64 bytes */
typedef struct __attribute__((packed)) {
    uint64_t sar;           /* +0  */
    uint64_t dar;           /* +8  */
    uint32_t block_ts_lo;   /* +16 */
    uint32_t block_ts_hi;   /* +20 */
    uint64_t llp;           /* +24 */
    uint32_t ctl_lo;        /* +32 */
    uint32_t ctl_hi;        /* +36 */
    uint32_t sstat;         /* +40 */
    uint32_t dstat;         /* +44 */
    uint32_t status_lo;     /* +48 */
    uint32_t status_hi;     /* +52 */
    uint32_t reserved_lo;   /* +56 */
    uint32_t reserved_hi;   /* +60 */
} rp1_dma_lli_t;

/* the controller fetches exactly 64 bytes per descriptor */
typedef char rp1_dma_lli_must_be_64_bytes[(sizeof(rp1_dma_lli_t) == 64) ? 1 : -1];

/* ------------------------------------------------------------------ state */

static uint8_t _ready;
static uint32_t _flags;
static ewokos_addr_t _clk_base;
static ewokos_addr_t _aout_base;
static ewokos_addr_t _dmac_base;

static ewokos_addr_t _ring_raw;       /* handle for dma_free() */
static uint32_t _slot_count;
static uint32_t _slot_frames;
static rp1_dma_lli_t* _lli;
static uint64_t _lli_bus;
static uint32_t* _slots_virt;
static uint64_t _slots_bus;
static uint32_t _lli_ctl_lo;
static uint32_t _lli_ctl_hi;
static uint32_t _lli_block_ts;
static bool _running;

static inline ewokos_addr_t dmac_chan_base(uint32_t id) {
    return _dmac_base + DMAC_COMMON_REG_LEN + (ewokos_addr_t)id * DMAC_CHAN_REG_LEN;
}

static inline uint32_t clk_get32(uint32_t off) { return get32(_clk_base + off); }
static inline void clk_put32(uint32_t off, uint32_t v) { put32(_clk_base + off, v); }
static inline uint32_t aout_get32(uint32_t off) { return get32(_aout_base + off); }
static inline void aout_put32(uint32_t off, uint32_t v) { put32(_aout_base + off, v); }
static inline uint32_t dmac_get32(uint32_t off) { return get32(_dmac_base + off); }
static inline void dmac_put32(uint32_t off, uint32_t v) { put32(_dmac_base + off, v); }

/* __ffs(): index of the lowest set bit, 0 when v == 0 */
static inline uint32_t bit_ffs(uint64_t v) {
    uint32_t i = 0;
    if (v == 0)
        return 0;
    while ((v & 1ULL) == 0) {
        v >>= 1;
        i++;
    }
    return i;
}

uint32_t rp1_audio_pack_frame(int16_t left, int16_t right) {
    return ((uint32_t)(uint16_t)left) | (((uint32_t)(uint16_t)right) << 16);
}

/* ------------------------------------------------------------ clock setup */

/* rp1_pll_core_set_rate() + rp1_pll_core_on() for PLL_AUDIO_CORE */
static int pll_audio_core_enable(void) {
    /* "Disable dividers to start with." */
    clk_put32(PLL_AUDIO_FBDIV_INT, 0);
    clk_put32(PLL_AUDIO_FBDIV_FRAC, 0);

    /* a fractional feedback divider needs the DSM powered up, so PWR = 0
       rather than PLL_PWR_DSMPD */
    clk_put32(PLL_AUDIO_PWR, 0);
    clk_put32(PLL_AUDIO_FBDIV_INT, PLL_AUDIO_FBDIV_INT_VAL);
    clk_put32(PLL_AUDIO_FBDIV_FRAC, PLL_AUDIO_FBDIV_FRAC_VAL);

    /* refdiv = 1; writing CS is what kicks the core */
    clk_put32(PLL_AUDIO_CS, clk_get32(PLL_AUDIO_CS) | (1u << PLL_CS_REFDIV_SHIFT));

    for (uint32_t n = 0; n < PLL_LOCK_POLL_MAX; n++) {
        if (clk_get32(PLL_AUDIO_CS) & PLL_CS_LOCK)
            return RP1_AUDIO_ERR_NONE;
        usleep(PLL_LOCK_POLL_US);
    }
    klog("rp1-audio: PLL_AUDIO_CORE never locked cs=%08x pwr=%08x int=%u frac=%u\n",
            clk_get32(PLL_AUDIO_CS), clk_get32(PLL_AUDIO_PWR),
            clk_get32(PLL_AUDIO_FBDIV_INT), clk_get32(PLL_AUDIO_FBDIV_FRAC));
    return RP1_AUDIO_ERR_CLOCK;
}

/* rp1_pll_divider_set_rate() + rp1_pll_divider_on() for PLL_AUDIO_SEC */
static void pll_audio_sec_enable(void) {
    uint32_t sec = clk_get32(PLL_AUDIO_SEC);
    if (!(sec & PLL_SEC_IMPL)) {
        /* rp1_pll_divider_on() only WARNs here; the SEC output may still be
           wired up by the RP1 firmware, so carry on */
        klog("rp1-audio: PLL_AUDIO_SEC impl bit clear ctrl=%08x\n", sec);
    }
    sec = (sec & ~PLL_SEC_DIV_MASK) |
            FIELD(PLL_AUDIO_SEC_DIV_VAL, PLL_SEC_DIV_MASK, PLL_SEC_DIV_SHIFT);
    /* "Must keep the divider in reset to change the value." */
    clk_put32(PLL_AUDIO_SEC, sec | PLL_SEC_RST);
    clk_put32(PLL_AUDIO_SEC, sec & ~PLL_SEC_RST);
}

/* rp1_clock_set_rate_and_parent() + rp1_clock_on() for CLK_AUDIO_OUT */
static void clk_audio_out_enable(void) {
    clk_put32(CLK_AUDIO_OUT_DIV_INT, CLK_AUDIO_OUT_DIV_VAL);
    uint32_t ctrl = clk_get32(CLK_AUDIO_OUT_CTRL);
    ctrl &= ~CLK_CTRL_AUXSRC_MASK;
    ctrl |= FIELD(CLK_AUDIO_OUT_AUXSRC_PLL_SEC,
            CLK_CTRL_AUXSRC_MASK, CLK_CTRL_AUXSRC_SHIFT);
    clk_put32(CLK_AUDIO_OUT_CTRL, ctrl);
    clk_put32(CLK_AUDIO_OUT_CTRL, clk_get32(CLK_AUDIO_OUT_CTRL) | CLK_CTRL_ENABLE);
}

/*
 * CLK_DMA is the DMAC core clock. It is not in rp1.dtsi's assigned-clocks
 * list, so the RP1 firmware normally leaves it running; only fall back to
 * the xosc parent when it is off, and never touch a live divider.
 */
static void clk_dma_enable(void) {
    uint32_t ctrl = clk_get32(CLK_DMA_CTRL);
    if (ctrl & CLK_CTRL_ENABLE)
        return;
    clk_put32(CLK_DMA_DIV_INT, 1);
    ctrl &= ~CLK_CTRL_AUXSRC_MASK;
    ctrl |= FIELD(CLK_DMA_AUXSRC_XOSC, CLK_CTRL_AUXSRC_MASK, CLK_CTRL_AUXSRC_SHIFT);
    clk_put32(CLK_DMA_CTRL, ctrl);
    clk_put32(CLK_DMA_CTRL, clk_get32(CLK_DMA_CTRL) | CLK_CTRL_ENABLE);
}

/* ------------------------------------------------------------ audio block */

/*
 * rp1_aout.c audio_init(). The block comment there is worth repeating: the
 * hardware was tuned for 48 kHz with 40x oversampling and 40-level two-sided
 * PWM, i.e. 48000 * 40 * 80 = 153.6 MHz, and changing those settings is not
 * recommended because the filter only leaves ~2.2 dB of headroom.
 */
static void audio_block_init(void) {
    /* clamp to +/- (32767 * 40 / 64) before quantization */
    uint32_t clamp = FIELD(20479, AUDIO_OUT_QCLAMP_MAX_MASK, AUDIO_OUT_QCLAMP_MAX_SHIFT) |
            FIELD((uint16_t)(-20479), AUDIO_OUT_QCLAMP_MIN_MASK, AUDIO_OUT_QCLAMP_MIN_SHIFT);
    aout_put32(AUDIO_OUT_QCLAMP_LEFT, clamp);
    aout_put32(AUDIO_OUT_QCLAMP_RIGHT, clamp);

    aout_put32(AUDIO_OUT_PWMCTL_LEFT, 0);
    aout_put32(AUDIO_OUT_PWMCTL_RIGHT, 0);
    /* Range = 39 */
    aout_put32(AUDIO_OUT_PWMRANGE_LEFT, 0x27);
    aout_put32(AUDIO_OUT_PWMRANGE_RIGHT, 0x27);

    /* bias = 20 (half FSD), quantize to 5+1 bits */
    uint32_t sdm = FIELD(0x14, AUDIO_OUT_SDMCTL_BIAS_MASK, AUDIO_OUT_SDMCTL_BIAS_SHIFT) |
            AUDIO_OUT_SDMCTL_CLAMP_EN | AUDIO_OUT_SDMCTL_DITHER_EN |
            FIELD(5, AUDIO_OUT_SDMCTL_BITWIDTH_MASK, AUDIO_OUT_SDMCTL_BITWIDTH_SHIFT);
    aout_put32(AUDIO_OUT_SDMCTL_LEFT, sdm);
    aout_put32(AUDIO_OUT_SDMCTL_RIGHT, sdm);

    /* ~300 ms ramp = 12k*40 samples to FSD/2 => step size 1, interval 13 */
    uint32_t mute = FIELD(1, AUDIO_OUT_MUTE_STEP_MASK, AUDIO_OUT_MUTE_STEP_SHIFT) |
            FIELD(13, AUDIO_OUT_MUTE_PERIOD_MASK, AUDIO_OUT_MUTE_PERIOD_SHIFT);
    aout_put32(AUDIO_OUT_MUTE_CTRL_LEFT, mute);
    aout_put32(AUDIO_OUT_MUTE_CTRL_RIGHT, mute);

    /* DMA flow control with the threshold at half the FIFO depth */
    aout_put32(AUDIO_OUT_FIFO_CONTROL,
            FIELD(2, AUDIO_OUT_FIFO_DWELL_MASK, AUDIO_OUT_FIFO_DWELL_SHIFT) |
            FIELD(0x10, AUDIO_OUT_FIFO_THRESHOLD_MASK, AUDIO_OUT_FIFO_THRESHOLD_SHIFT) |
            AUDIO_OUT_FIFO_DMA_DREQ_EN);
}

/* rp1_aout.c audio_startup() */
static void audio_block_startup(void) {
    /* CIC rate 10, for an overall upsampling ratio of 40 */
    uint32_t val = FIELD(0xa, AUDIO_OUT_CTRL_CIC_RATE_MASK, AUDIO_OUT_CTRL_CIC_RATE_SHIFT) |
            AUDIO_OUT_CTRL_LEFT_CH_ENABLE | AUDIO_OUT_CTRL_RIGHT_CH_ENABLE;
    if (_flags & RP1_AUDIO_F_CHANNEL_SWAP)
        val |= AUDIO_OUT_CTRL_CHANNEL_SWAP;
    aout_put32(AUDIO_OUT_CTRL, val);
    (void)aout_get32(AUDIO_OUT_CTRL);   /* synchronization delay */

    /* press the "go" button */
    val |= AUDIO_OUT_CTRL_PERIPH_EN;
    aout_put32(AUDIO_OUT_CTRL, val);
    (void)aout_get32(AUDIO_OUT_CTRL);   /* FIFO reset release delay */

    /* poke zeroes in to avoid undefined values on underrun */
    aout_put32(AUDIO_OUT_SAMPLE_FIFO, 0);
}

/* rp1_aout.c audio_muting() */
static void audio_muting(uint32_t flag) {
    uint32_t val = FIELD(1, AUDIO_OUT_MUTE_STEP_MASK, AUDIO_OUT_MUTE_STEP_SHIFT) |
            FIELD(13, AUDIO_OUT_MUTE_PERIOD_MASK, AUDIO_OUT_MUTE_PERIOD_SHIFT);
    aout_put32(AUDIO_OUT_MUTE_CTRL_LEFT, val);
    aout_put32(AUDIO_OUT_MUTE_CTRL_RIGHT, val);

    val |= flag;
    aout_put32(AUDIO_OUT_MUTE_CTRL_LEFT, val);
    aout_put32(AUDIO_OUT_MUTE_CTRL_RIGHT, val);
    (void)aout_get32(AUDIO_OUT_MUTE_CTRL_RIGHT);   /* synchronization delay */
}

/* rp1_aout.c audio_mute_sync(): wait for both ramp FSMs to settle */
static void audio_mute_sync(void) {
    uint32_t n;
    for (n = 0; n < MUTE_SYNC_POLL_MAX; n++) {
        if ((aout_get32(AUDIO_OUT_MUTE_CTRL_LEFT) & AUDIO_OUT_MUTE_FSM_BUSY_MASK) == 0)
            break;
        usleep(MUTE_SYNC_POLL_US);
    }
    for (; n < MUTE_SYNC_POLL_MAX; n++) {
        if ((aout_get32(AUDIO_OUT_MUTE_CTRL_RIGHT) & AUDIO_OUT_MUTE_FSM_BUSY_MASK) == 0)
            break;
        usleep(MUTE_SYNC_POLL_US);
    }
}

static void audio_pins_init(void) {
    bcm2712_gpio_init();
    /*
     * pinctrl-rp1.c: PIN(12, pwm0, dpi, uart4, i2c2, aaud, gpio, proc_rio,
     * pio, spi5) and the same table for pin 13, so "aaud" is the fifth
     * alternative, i.e. funcsel 4. rp1.dtsi's rp1_audio_out_12_13 state
     * asks for bias-disable.
     */
    bcm2712_gpio_pull(RP1_AUDIO_GPIO_LEFT, GPIO_PULL_NONE);
    bcm2712_gpio_pull(RP1_AUDIO_GPIO_RIGHT, GPIO_PULL_NONE);
    bcm2712_gpio_config(RP1_AUDIO_GPIO_LEFT, GPIO_FUNC_ALTF4);
    bcm2712_gpio_config(RP1_AUDIO_GPIO_RIGHT, GPIO_FUNC_ALTF4);
}

/* ------------------------------------------------------------------- DMAC */

static void dmac_chan_enable(uint32_t id) {
    uint32_t val = dmac_get32(DMAC_CHEN);
    val |= (1u << (id + DMAC_CHAN_EN_SHIFT)) | (1u << (id + DMAC_CHAN_EN_WE_SHIFT));
    dmac_put32(DMAC_CHEN, val);
}

static void dmac_chan_disable(uint32_t id) {
    uint32_t val = dmac_get32(DMAC_CHEN);
    val &= ~(1u << (id + DMAC_CHAN_EN_SHIFT));
    val |= (1u << (id + DMAC_CHAN_EN_WE_SHIFT));
    dmac_put32(DMAC_CHEN, val);
}

static bool dmac_chan_enabled(uint32_t id) {
    return (dmac_get32(DMAC_CHEN) & (1u << (id + DMAC_CHAN_EN_SHIFT))) != 0;
}

/* axi_dma_hw_init(), minus the parts that need a linux dma mask */
static int dmac_hw_init(void) {
    dmac_put32(DMAC_RESET, 1);
    uint32_t n;
    for (n = 0; n < DMAC_RESET_POLL_MAX; n++) {
        if (dmac_get32(DMAC_RESET) == 0)
            break;
    }
    if (n >= DMAC_RESET_POLL_MAX) {
        klog("rp1-audio: DMAC failed to reset\n");
        return RP1_AUDIO_ERR_DMA_RST;
    }

    for (uint32_t i = 0; i < DMAC_NUM_CHANNELS; i++) {
        ewokos_addr_t ch = dmac_chan_base(i);
        /* polled driver: no channel interrupt may reach the RP1 IRQ line */
        put32(ch + CH_INTSTATUS_ENA, 0);
        put32(ch + CH_INTSIGNAL_ENA, 0);
        put32(ch + CH_INTCLEAR, 0xffffffffu);
        dmac_chan_disable(i);
    }

    /* axi_dma_resume(): DMAC_CFG |= DMAC_EN_MASK. INT_EN stays clear. */
    dmac_put32(DMAC_CFG, (dmac_get32(DMAC_CFG) & ~DMAC_INT_EN_MASK) | DMAC_EN_MASK);
    return RP1_AUDIO_ERR_NONE;
}

/*
 * dw_axi_dma_set_hw_desc() for DMA_MEM_TO_DEV, specialised to the audio_out
 * FIFO. rp1.dtsi declares snps,data-width = <4> (a 128-bit memory bus),
 * dma-maxburst = <4> with a 4-byte access width on the device side.
 */
static void dmac_build_ctl(uint64_t mem_bus, uint32_t len) {
    uint32_t data_width = 1u << 4;              /* BIT(snps,data-width) = 16 */
    uint32_t mem_width = bit_ffs((uint64_t)data_width | mem_bus | len);
    /* axi_dma_encode_msize(): 16 -> BURST_TRANS_LEN_16, 4 -> _4 */
    uint32_t reg_width = bit_ffs(4);            /* __ffs(dst_addr_width) */

    _lli_ctl_lo =
            (reg_width << CH_CTL_L_DST_WIDTH_POS) |
            (mem_width << CH_CTL_L_SRC_WIDTH_POS) |
            (BURST_TRANS_LEN_4 << CH_CTL_L_DST_MSIZE_POS) |
            (BURST_TRANS_LEN_16 << CH_CTL_L_SRC_MSIZE_POS) |
            (CH_CTL_L_NOINC << CH_CTL_L_DST_INC_POS) |
            (CH_CTL_L_INC << CH_CTL_L_SRC_INC_POS);
    /* DST_MAST/SRC_MAST stay clear: snps,dma-masters = <1>, so AXI0 only */

    _lli_block_ts = len >> mem_width;

    /*
     * LLI_LAST deliberately stays clear, see rp1_audio.h. ARLEN/AWLEN are
     * capped at 8 beats, the channel-0 snps,axi-max-burst-len value, encoded
     * as beats-1 the way Circle does.
     */
    _lli_ctl_hi = CH_CTL_H_LLI_VALID |
            CH_CTL_H_ARLEN_EN | (ARWLEN_8 << CH_CTL_H_ARLEN_POS) |
            CH_CTL_H_AWLEN_EN | (ARWLEN_8 << CH_CTL_H_AWLEN_POS);
}

/* ---------------------------------------------------------------- public */

static int audio_map_windows(void) {
    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;

    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
        klog("rp1-audio: main MMIO map failed\n");
        return RP1_AUDIO_ERR_MAP;
    }
    ewokos_addr_t rp1_vbase = _mmio_base + PI5_RP1_WIN_OFF;
    if (syscall3(SYS_MEM_MAP, rp1_vbase, PI5_RP1_PHY, PI5_RP1_WIN_SIZE)
            != rp1_vbase) {
        klog("rp1-audio: RP1 window map failed\n");
        return RP1_AUDIO_ERR_MAP;
    }

    _clk_base = _mmio_base + RP1_CLK_OFF;
    _aout_base = _mmio_base + RP1_AOUT_OFF;
    _dmac_base = _mmio_base + RP1_DMAC_OFF;
    return RP1_AUDIO_ERR_NONE;
}

int rp1_audio_init(uint32_t flags) {
    if (_ready) {
        _flags = flags;
        return RP1_AUDIO_ERR_NONE;
    }
    _flags = flags;

    int ret = audio_map_windows();
    if (ret != RP1_AUDIO_ERR_NONE)
        return ret;

    /* RP1's BARs and the PCIe2 link must be live before any of this reads */
    ret = bcm2712_rp1_init();
    if (ret != 0) {
        klog("rp1-audio: rp1 pcie init failed ret=%d\n", ret);
        return RP1_AUDIO_ERR_RP1;
    }

    ret = pll_audio_core_enable();
    if (ret != RP1_AUDIO_ERR_NONE)
        return ret;
    pll_audio_sec_enable();
    clk_audio_out_enable();
    clk_dma_enable();

    ret = dmac_hw_init();
    if (ret != RP1_AUDIO_ERR_NONE)
        return ret;

    audio_block_init();
    audio_block_startup();
    /* leave the output muted until playback actually starts */
    audio_muting(AUDIO_OUT_MUTE_INIT_MUTE);
    audio_pins_init();

    _ready = 1;
    return RP1_AUDIO_ERR_NONE;
}

int rp1_audio_setup_ring(uint32_t slots, uint32_t slot_frames) {
    if (!_ready)
        return RP1_AUDIO_ERR_STATE;
    if (_running)
        return RP1_AUDIO_ERR_STATE;
    /* the guard band needs room, and the ring has to be worth polling */
    if (slots < (2u * RP1_AUDIO_GUARD_SLOTS + 2u) || slots > 64u)
        return RP1_AUDIO_ERR_PARAM;
    /* a multiple of 16 frames keeps every slot buffer 64-byte aligned, so
       the DMAC can use the full 128-bit memory data width */
    if (slot_frames < 16u || slot_frames > 0x10000u || (slot_frames & 15u) != 0u)
        return RP1_AUDIO_ERR_PARAM;

    if (_lli != NULL)
        rp1_audio_teardown_ring();

    uint32_t lli_bytes = slots * (uint32_t)sizeof(rp1_dma_lli_t);
    uint32_t buf_bytes = slots * slot_frames * RP1_AUDIO_FRAME_WORDS * 4u;
    uint32_t total = lli_bytes + buf_bytes;

    /* over-allocate one alignment unit, dma_alloc() only guarantees a page */
    ewokos_addr_t raw = dma_alloc(0, total + RP1_LLI_ALIGN);
    if (raw == 0) {
        klog("rp1-audio: ring dma_alloc(%u) failed\n", total + RP1_LLI_ALIGN);
        return RP1_AUDIO_ERR_DMA_MEM;
    }
    ewokos_addr_t phy = dma_phy_addr(0, raw);
    if (phy == 0) {
        klog("rp1-audio: ring dma_phy_addr failed\n");
        dma_free(0, raw);
        return RP1_AUDIO_ERR_DMA_MEM;
    }

    /* the DMA window is mapped linearly, so one pad aligns both views */
    uint32_t pad = (uint32_t)((RP1_LLI_ALIGN - (phy & (RP1_LLI_ALIGN - 1u)))
            & (RP1_LLI_ALIGN - 1u));
    _ring_raw = raw;
    _lli = (rp1_dma_lli_t*)(raw + pad);
    _lli_bus = (uint64_t)(phy + pad) + RP1_RAM_BUS_OFF;
    _slots_virt = (uint32_t*)((uint8_t*)_lli + lli_bytes);
    _slots_bus = _lli_bus + lli_bytes;
    _slot_count = slots;
    _slot_frames = slot_frames;

    memset(_lli, 0, lli_bytes);
    memset(_slots_virt, 0, buf_bytes);

    dmac_build_ctl(_slots_bus, buf_bytes / slots);
    /* snps,block-size is 0x40000 for every channel */
    if (_lli_block_ts == 0 || _lli_block_ts > 0x40000u) {
        klog("rp1-audio: slot block_ts %u out of range\n", _lli_block_ts);
        rp1_audio_teardown_ring();
        return RP1_AUDIO_ERR_PARAM;
    }
    return RP1_AUDIO_ERR_NONE;
}

void rp1_audio_teardown_ring(void) {
    if (_running)
        rp1_audio_stop();
    _lli = NULL;
    _lli_bus = 0;
    _slots_virt = NULL;
    _slots_bus = 0;
    _slot_count = 0;
    _slot_frames = 0;
    if (_ring_raw != 0) {
        dma_free(0, _ring_raw);
        _ring_raw = 0;
    }
}

int rp1_audio_start(void) {
    if (!_ready)
        return RP1_AUDIO_ERR_STATE;
    if (_lli == NULL || _slot_count == 0)
        return RP1_AUDIO_ERR_STATE;

    /* restart cleanly if a previous run is still walking the ring */
    if (dmac_chan_enabled(AUDIO_DMA_CHANNEL))
        dmac_chan_disable(AUDIO_DMA_CHANNEL);

    /* a pending mute ramp must finish before the output is re-enabled */
    audio_mute_sync();

    uint32_t slot_bytes = _slot_frames * RP1_AUDIO_FRAME_WORDS * 4u;
    uint64_t fifo_bus = RP1_PERIPH_BUS(RP1_AOUT_REG_OFF + AUDIO_OUT_SAMPLE_FIFO);
    for (uint32_t i = 0; i < _slot_count; i++) {
        rp1_dma_lli_t* lli = &_lli[i];
        lli->sar = _slots_bus + (uint64_t)i * slot_bytes;
        lli->dar = fifo_bus;
        lli->block_ts_lo = _lli_block_ts - 1u;
        lli->block_ts_hi = 0;
        /* closed ring: the last descriptor links back to the first */
        lli->llp = _lli_bus +
                (uint64_t)(((i + 1u) % _slot_count) * sizeof(rp1_dma_lli_t));
        lli->ctl_lo = _lli_ctl_lo;
        lli->ctl_hi = _lli_ctl_hi;
        lli->sstat = 0;
        lli->dstat = 0;
        lli->status_lo = 0;
        lli->status_hi = 0;
    }
    aout_dmb();

    /*
     * axi_chan_block_xfer_start(): both multi-block types are linked list,
     * the DMAC is the flow controller for a memory-to-peripheral transfer,
     * and both handshakes are hardware. dst_per is the RP1_DMA_AUDIO_OUT
     * request line because rp1 has no APB register block.
     */
    ewokos_addr_t ch = dmac_chan_base(AUDIO_DMA_CHANNEL);
    put32(ch + CH_CFG_L,
            (MBLK_TYPE_LL << CH_CFG_L_DST_MULTBLK_POS) |
            (MBLK_TYPE_LL << CH_CFG_L_SRC_MULTBLK_POS) |
            (RP1_DMA_AUDIO_OUT << CH_CFG2_L_DST_PER_POS));
    put32(ch + CH_CFG_H,
            (TT_FC_MEM_TO_PER_DMAC << CH_CFG2_H_TT_FC_POS) |
            (HS_SEL_HW << CH_CFG2_H_HS_SEL_SRC_POS) |
            (HS_SEL_HW << CH_CFG2_H_HS_SEL_DST_POS) |
            (AUDIO_DMA_PRIORITY << CH_CFG2_H_PRIORITY_POS));
    put32(ch + CH_SAR, 0);
    put32(ch + CH_DAR, 0);
    put32(ch + CH_BLOCK_TS, 0);
    put32(ch + CH_CTL_L, 0);
    put32(ch + CH_CTL_H, 0);
    put32(ch + CH_INTSTATUS_ENA, 0);
    put32(ch + CH_INTSIGNAL_ENA, 0);
    put32(ch + CH_INTCLEAR, 0xffffffffu);

    /* prime the FIFO so the modulator never starts on an undefined sample */
    aout_put32(AUDIO_OUT_SAMPLE_FIFO, 0);

    put32(ch + CH_LLP_L, (uint32_t)_lli_bus);
    put32(ch + CH_LLP_H, (uint32_t)(_lli_bus >> 32));
    aout_dmb();

    dmac_chan_enable(AUDIO_DMA_CHANNEL);
    _running = dmac_chan_enabled(AUDIO_DMA_CHANNEL);
    if (!_running) {
        klog("rp1-audio: channel %u refused to start chen=%08x\n",
                AUDIO_DMA_CHANNEL, dmac_get32(DMAC_CHEN));
        return RP1_AUDIO_ERR_STATE;
    }

    audio_muting(AUDIO_OUT_MUTE_INIT_UNMUTE);
    return RP1_AUDIO_ERR_NONE;
}

void rp1_audio_stop(void) {
    if (!_ready)
        return;
    /* ramp the output to the bias level first, so stopping cannot click */
    audio_muting(AUDIO_OUT_MUTE_INIT_MUTE);

    if (dmac_chan_enabled(AUDIO_DMA_CHANNEL)) {
        dmac_chan_disable(AUDIO_DMA_CHANNEL);
        for (uint32_t n = 0; n < DMAC_STOP_POLL_MAX; n++) {
            if (!dmac_chan_enabled(AUDIO_DMA_CHANNEL))
                break;
            usleep(DMAC_STOP_POLL_US);
        }
        if (dmac_chan_enabled(AUDIO_DMA_CHANNEL))
            klog("rp1-audio: channel %u failed to stop chen=%08x status=%08x\n",
                    AUDIO_DMA_CHANNEL, dmac_get32(DMAC_CHEN),
                    get32(dmac_chan_base(AUDIO_DMA_CHANNEL) + CH_STATUS));
    }
    /* "Push a zero sample (assuming DMA has stopped already)" */
    aout_put32(AUDIO_OUT_SAMPLE_FIFO, 0);
    _running = false;
}

bool rp1_audio_running(void) {
    if (!_running || !_ready)
        return false;
    /* the ring is free-running, so a dropped enable bit means it died */
    return dmac_chan_enabled(AUDIO_DMA_CHANNEL);
}

uint32_t rp1_audio_slots(void) { return _slot_count; }
uint32_t rp1_audio_slot_frames(void) { return _slot_frames; }

uint32_t* rp1_audio_slot_buffer(uint32_t slot) {
    if (_slots_virt == NULL || slot >= _slot_count)
        return NULL;
    return _slots_virt + (size_t)slot * _slot_frames * RP1_AUDIO_FRAME_WORDS;
}

int rp1_audio_hw_slot(void) {
    if (!_ready || _lli == NULL || _slot_count == 0)
        return -1;
    if (!dmac_chan_enabled(AUDIO_DMA_CHANNEL))
        return -1;

    ewokos_addr_t ch = dmac_chan_base(AUDIO_DMA_CHANNEL);
    /*
     * CH_LLP is the address of the descriptor the controller is working on;
     * linux reads it back the same way in axi_chan_block_xfer_complete().
     * Bits [1:0] select the AXI master used for descriptor fetches (lms).
     * The high word never changes because the whole ring sits inside one
     * allocation, so the two reads cannot tear into a bogus index.
     */
    uint64_t llp = ((uint64_t)get32(ch + CH_LLP_H) << 32) | get32(ch + CH_LLP_L);
    llp &= ~3ULL;
    if (llp < _lli_bus)
        return -1;
    uint64_t off = llp - _lli_bus;
    uint64_t span = (uint64_t)_slot_count * sizeof(rp1_dma_lli_t);
    if (off >= span || (off % sizeof(rp1_dma_lli_t)) != 0)
        return -1;
    return (int)(off / sizeof(rp1_dma_lli_t));
}

bool rp1_audio_slot_writable(uint32_t slot) {
    if (_slot_count == 0 || slot >= _slot_count)
        return false;
    int hw = rp1_audio_hw_slot();
    if (hw < 0)
        return false;
    /* forward distance from the slot the controller is on */
    uint32_t d = (slot + _slot_count - (uint32_t)hw) % _slot_count;
    /*
     * d == 0 is in flight and d == 1 may already have been fetched into the
     * shadow registers; d == _slot_count-1 is the block that just finished,
     * whose status write-back may still be landing. Anything else is at
     * least one whole slot period away, which is milliseconds.
     */
    return d > RP1_AUDIO_GUARD_SLOTS &&
            d < (_slot_count - RP1_AUDIO_GUARD_SLOTS);
}

void rp1_audio_slot_commit(uint32_t slot) {
    if (_lli == NULL || slot >= _slot_count)
        return;
    /* samples first, then the descriptor that publishes them */
    aout_dmb();
    /*
     * Rewrite the whole word rather than OR-ing LLI_VALID back in: the
     * controller writes the block status into the LLI when it consumes it,
     * which clears LLI_VALID, and a plain write cannot race with that the
     * way a read-modify-write could.
     */
    _lli[slot].ctl_hi = _lli_ctl_hi;
    aout_dmb();
}

void rp1_audio_rearm_all(void) {
    if (_lli == NULL || _slot_count == 0)
        return;
    for (uint32_t i = 0; i < _slot_count; i++) {
        if (rp1_audio_slot_writable(i))
            _lli[i].ctl_hi = _lli_ctl_hi;
    }
    aout_dmb();
}
