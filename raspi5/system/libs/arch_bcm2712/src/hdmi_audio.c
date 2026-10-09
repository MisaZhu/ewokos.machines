/*
 * HDMI (VC4 MAI) + dma40 cyclic ring playback backend for BCM2712 (Raspberry
 * Pi 5). See hdmi_audio.h for the caller-facing model.
 *
 * Everything here is polled: no DMA interrupt reaches user space, so the channel
 * is left walking a closed chain of 40-bit control blocks forever and its
 * position is read back from the CB register.
 *
 * Register maps and programming sequences come from:
 *   drivers/gpu/drm/vc4/vc4_hdmi.c        MAI bring-up, N/CTS, audio infoframe
 *   drivers/gpu/drm/vc4/vc4_hdmi_regs.h   vc6_hdmi_hdmi0_fields[], MAI bitfields
 *   drivers/dma/bcm2835-dma.c             dma40 engine (is_40bit_channel path)
 *   arch/arm64/boot/dts/broadcom/bcm2712.dtsi  addresses, axi identity
 *                                         dma-ranges, dma40 @ 0x10_00010600
 * Circle's CHDMISoundBaseDevice / CSoundBaseDevice corroborate the fixed
 * 108 MHz MAI clock and the software IEC958 subframe packing on real Pi 5.
 */
#include <arch/bcm2712/hdmi_audio.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/mailbox.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <string.h>
#include <unistd.h>

#define hdmi_dmb() __asm__ volatile("dmb sy" ::: "memory")

/* ------------------------------------------------------- field helpers */

#define BIT(n)              (1U << (n))
#define VC4_MASK(high, low) (((uint32_t)(~0U) >> (31 - ((high) - (low)))) << (low))
#define VC4_SHIFT(mask)     (__builtin_ctz(mask))
#define VC4_SET_FIELD(value, mask) ((((uint32_t)(value)) << VC4_SHIFT(mask)) & (mask))

/* ------------------------------------------- HDMI register block offsets */

/*
 * Offsets inside the main MMIO window (relative to _mmio_base). Every HDMI0
 * bank sits below 0x04000000, so all of them are reached through the window
 * the kernel already maps; only the legacy dma32 engine needs its own
 * SYS_MEM_MAP (see PI5_DMA32_* in mmio.h and the kernel whitelist).
 */
#define PI5_HVS_OFF         0x00580000U
#define PI5_HDMI0_HDMI_OFF  0x00701400U   /* "hdmi" bank: packets, clock regen */
#define PI5_HDMI0_HD_OFF    0x00720000U   /* "hd" bank: MAI FIFO control */
#define PI5_HDMI0_RAM_OFF   0x00703800U   /* VC5_RAM: packet RAM */

#define SCALER6D0_HVS_ID    0x000000fcU

/* "hd" bank MAI registers (vc6_hdmi_hdmi0_fields[], VC4_HD_REG) */
#define HDMI_MAI_CTL        0x0010
#define HDMI_MAI_THR        0x0014
#define HDMI_MAI_FMT        0x0018
#define HDMI_MAI_DATA       0x001c   /* FIFO write port: the dma40 destination */
#define HDMI_MAI_SMP        0x0020

/* "hdmi" bank registers (VC4_HDMI_REG) */
#define HDMI_MAI_CHANNEL_MAP      0x0a4
#define HDMI_MAI_CONFIG           0x0a8
#define HDMI_AUDIO_PACKET_CONFIG  0x0c0
#define HDMI_RAM_PACKET_CONFIG    0x0c4
#define HDMI_RAM_PACKET_STATUS    0x0cc
#define HDMI_CRP_CFG              0x0d0
#define HDMI_CTS_0                0x0d4
#define HDMI_CTS_1                0x0d8
#define HDMI_HOTPLUG              0x1c8

/* VC5_RAM packet RAM: packet N lives at RAM_PACKET_START + 0x24 * N */
#define HDMI_RAM_PACKET_START     0x000
#define VC4_HDMI_PACKET_STRIDE    0x24
#define HDMI_INFOFRAME_TYPE_AUDIO 0x84
#define HDMI_AUDIO_PACKET_ID      (HDMI_INFOFRAME_TYPE_AUDIO - 0x80)   /* 4 */

/* MAI_CTL bits */
#define VC4_HD_MAI_CTL_DLATE      BIT(15)
#define VC4_HD_MAI_CTL_CHALIGN    BIT(13)
#define VC4_HD_MAI_CTL_WHOLSMP    BIT(12)
#define VC4_HD_MAI_CTL_EMPTY      BIT(10)   /* read-only: the FIFO has drained */
#define VC4_HD_MAI_CTL_FLUSH      BIT(9)
#define VC4_HD_MAI_CTL_PAREN      BIT(8)
#define VC4_HD_MAI_CTL_CHNUM_MASK VC4_MASK(7, 4)
#define VC4_HD_MAI_CTL_ENABLE     BIT(3)
#define VC4_HD_MAI_CTL_ERRORE     BIT(2)
#define VC4_HD_MAI_CTL_ERRORF     BIT(1)
#define VC4_HD_MAI_CTL_RESET      BIT(0)

/* MAI_THR fields: the D0 step of the vc6 core shifts them all up by one bit */
#define VC4_HD_MAI_THR_PANICHIGH_MASK    VC4_MASK(29, 24)
#define VC4_HD_MAI_THR_PANICLOW_MASK     VC4_MASK(21, 16)
#define VC4_HD_MAI_THR_DREQHIGH_MASK     VC4_MASK(13, 8)
#define VC4_HD_MAI_THR_DREQLOW_MASK      VC4_MASK(5, 0)
#define VC4_D0_HD_MAI_THR_PANICHIGH_MASK VC4_MASK(29, 23)
#define VC4_D0_HD_MAI_THR_PANICLOW_MASK  VC4_MASK(21, 15)
#define VC4_D0_HD_MAI_THR_DREQHIGH_MASK  VC4_MASK(13, 7)
#define VC4_D0_HD_MAI_THR_DREQLOW_MASK   VC4_MASK(6, 0)

/* MAI_FMT fields */
#define VC4_HD_MAI_FMT_AUDIO_FORMAT_MASK VC4_MASK(23, 16)
#define VC4_HD_MAI_FMT_SAMPLE_RATE_MASK  VC4_MASK(15, 8)
#define VC4_HDMI_MAI_FORMAT_PCM          2u

/* MAI_SMP: the sampling period converges to N / (M + 1) audio clocks */
#define VC4_HD_MAI_SMP_N_MASK  VC4_MASK(31, 8)
#define VC4_HD_MAI_SMP_M_MASK  VC4_MASK(7, 0)

/* MAI_CONFIG ("hdmi" bank) */
#define VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE BIT(27)
#define VC4_HDMI_MAI_CONFIG_BIT_REVERSE    BIT(26)
#define VC4_HDMI_MAI_CHANNEL_MASK_MASK     VC4_MASK(15, 0)

/* AUDIO_PACKET_CONFIG ("hdmi" bank) */
#define VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT      BIT(29)
#define VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS BIT(24)
#define VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER_MASK       VC4_MASK(13, 10)
#define VC4_HDMI_AUDIO_PACKET_CEA_MASK_MASK                 VC4_MASK(7, 0)

/* RAM packet engine + clock regeneration ("hdmi" bank) */
#define VC4_HDMI_RAM_PACKET_ENABLE     BIT(16)
#define VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN BIT(24)
#define VC4_HDMI_CRP_CFG_N_MASK        VC4_MASK(19, 0)
#define VC4_HDMI_HOTPLUG_CONNECTED     BIT(0)

/*
 * The vc6 MAI serial clock ("audio" clock) runs at a fixed 108 MHz (clk_108MHz
 * in bcm2712.dtsi); Circle hard-codes the same value. MAI_SMP divides it down
 * to the sample rate, so the *rate* never has to be queried.
 *
 * Unlike vc4 (where audio_clock == hsm_clock), vc5/vc6 keep a *separate*
 * "audio" clock: vc4_hdmi.c vc5_hdmi_init_resources() does
 * devm_clk_get(dev, "audio") and vc4_hdmi_runtime_resume() re-enables it with
 * clk_prepare_enable(vc4_hdmi->audio_clock) before the MAI is touched. On
 * BCM2712 that clock is "hdmi0-108MHz", a gate in the DVP block at SoC
 * 0x7c700000 (clk-bcm2711-dvp.c: DVP_HT_RPI_MISC_CONFIG bit 3,
 * CLK_GATE_SET_TO_DISABLE). The display bring-up only drives the HDMI DVP
 * register bank at 0x7c701000 and never opens this gate, so the fixed rate is
 * not enough: the gate has to be cleared here or the MAI FIFO logic has no
 * clock, never drains, never raises a DREQ, and the dma40 channel parks with
 * CS bit3 (DREQ) clear and CB frozen at CB0 forever.
 */
#define HDMI_AUDIO_CLOCK_HZ  108000000ULL

/* DVP clock/reset controller (brcm,brcm2711-dvp @ SoC 0x7c700000). Distinct
 * from the HDMI DVP register bank at 0x7c701000 (PI5_HDMI0_DVP_OFF). */
#define PI5_HDMI0_DVP_CLK_OFF       0x00700000U
#define DVP_HT_RPI_MISC_CONFIG      0x08U
/* CLK_GATE_SET_TO_DISABLE: clearing the bit enables "hdmi0-108MHz" */
#define DVP_MISC_HDMI0_108MHZ_GATE  BIT(3)

/* firmware mailbox clock query (native_hdmi.c uses the same tag/id) */
#define RPI_FIRMWARE_GET_CLOCK_RATE  0x00030002u
#define PI5_FW_CLK_HDMI0_PIXEL       9u

/* ------------------------------------------------- BCM2712 dma40 engine */

/*
 * The runtime probe settled it on real hardware: the legacy dma32 slice never
 * sees the HDMI MAI request line. Armed with PER_MAP 12 and 10 its CS bit3
 * (DREQ) stayed clear and CONBLK_AD froze at CB0, so not one sample reached the
 * FIFO. bcm2712.dtsi splits the one register file at 0x10_00010000 into two DT
 * nodes -- dma32 @ +0x000 (channels 0-5, mask 0x0035) and dma40 @ +0x600
 * (channels 6-11, mask 0x0fc0; the "6 40-bit channels DMA6..DMA11" of
 * bcm2835-dma.c) -- and rpi-6.18.y drives HDMI audio through dma40 precisely
 * because the MAI DREQ is only reachable from the 40-bit slice.
 *
 * dma40 channel 6 lives at 0x10_00010600 = PI5_DMA32_PHY + 0x600, inside the
 * same 64 KB window this driver already maps and the kernel already whitelists,
 * so only the channel index, register layout and control-block format change.
 *
 * Both DMA nodes hang off the "axi" bus whose dma-ranges are IDENTITY over the
 * full 40-bit space (bcm2712.dtsi: <0x00 0x0 0x00 0x0 0x10 0x0>, ...), so a
 * dma40 master addresses RAM at its raw physical address (NOT the legacy
 * 0xc0000000 alias the dma32 path wrongly used) and reaches the MAI FIFO at its
 * full physical 0x10_7c72001c (hdmi0 sits under "soc", whose ranges map
 * 0x7c000000 -> 0x10_7c000000). The upper 8 bits ride in SCB.srci / SCB.dsti.
 */

/* per-channel register stride (channels are 0x100 apart in the shared file) */
#define DMA_CHAN_STRIDE       0x100u

/* dma40 per-channel registers (bcm2835-dma.c BCM2711_DMA40_*) */
#define DMA40_CS              0x00
#define DMA40_CB              0x04   /* current control block, held as addr>>5 */
#define DMA40_DEBUG           0x0c
#define DMA40_TI              0x10
#define DMA40_SRC             0x14
#define DMA40_SRCI            0x18
#define DMA40_DEST            0x1c
#define DMA40_DESTI           0x20
#define DMA40_LEN             0x24
#define DMA40_NEXT_CB         0x28
#define DMA40_DEBUG2          0x2c

/* dma40 CS bits */
#define DMA40_ACTIVE          BIT(0)
#define DMA40_END             BIT(1)
#define DMA40_INT             BIT(2)
#define DMA40_DREQ            BIT(3)
#define DMA40_RD_PAUSED       BIT(4)
#define DMA40_WR_PAUSED       BIT(5)
#define DMA40_DREQ_PAUSED     BIT(6)
#define DMA40_PROT            (BIT(8) | BIT(9))   /* supervisor-mode access */
#define DMA40_ERR             BIT(10)
#define DMA40_QOS(x)          (((x) & 0x1f) << 16)
#define DMA40_PANIC_QOS(x)    (((x) & 0x1f) << 20)
#define DMA40_TRANSACTIONS    BIT(25)
#define DMA40_WAIT_FOR_WRITES BIT(28)
#define DMA40_DISDEBUG        BIT(29)
#define DMA40_ABORT           BIT(30)
#define DMA40_HALT            BIT(31)
#define DMA40_CS_FLAGS(x)     ((x) & (DMA40_QOS(15) | DMA40_PANIC_QOS(15) | \
        DMA40_WAIT_FOR_WRITES | DMA40_DISDEBUG))

/* dma40 DEBUG bits */
#define DMA40_DEBUG_RESET     BIT(23)

/* dma40 transfer-information (TI) bits */
#define DMA40_INTEN           BIT(0)
#define DMA40_TDMODE          BIT(1)
#define DMA40_WAIT_RESP       BIT(2)
#define DMA40_WAIT_RD_RESP    BIT(3)
#define DMA40_PER_MAP(x)      (((x) & 31) << 9)
#define DMA40_S_DREQ          BIT(14)
#define DMA40_D_DREQ          BIT(15)

/* dma40 SRCI / DSTI bits */
#define DMA40_BURST_LEN(x)    (((x) & 15) << 8)
#define DMA40_INC             BIT(12)
#define DMA40_SIZE_32         (0u << 13)
#define DMA40_SIZE_64         (1u << 13)
#define DMA40_SIZE_128        (2u << 13)
#define DMA40_SIZE_256        (3u << 13)
#define DMA40_IGNORE          BIT(15)

/*
 * Legacy bcm2835 "info" bits. MAI_DMA_INFO() is still spelled with them and
 * to_bcm2711_ti/srci/dsti() below translate it into the dma40 TI/SRCI/DSTI
 * encoding, exactly as bcm2835-dma.c does for a 40-bit channel.
 */
#define BCM2835_DMA_INT_EN    BIT(0)
#define BCM2835_DMA_WAIT_RESP BIT(3)
#define BCM2835_DMA_D_INC     BIT(4)
#define BCM2835_DMA_D_WIDTH   BIT(5)
#define BCM2835_DMA_D_DREQ    BIT(6)
#define BCM2835_DMA_S_INC     BIT(8)
#define BCM2835_DMA_S_WIDTH   BIT(9)   /* 128-bit source reads (Circle sets this) */
#define BCM2835_DMA_S_DREQ    BIT(10)
#define BCM2835_DMA_PER_MAP(x)          (((x) & 31) << 16)
#define BCM2835_DMA_BURST_LENGTH(x)     (((x) & 15) << 12)
#define BCM2835_DMA_GET_BURST_LENGTH(x) (((x) >> 12) & 15)

/*
 * MAI peripheral request line. bcm2712.dtsi fixes hdmi0 audio at DREQ 10
 * (<&dma32 10>) while Circle remaps it to 12 on the D0 stepping; the sources
 * disagree and the dma40 wiring is undocumented, so both are probed at runtime
 * (mai_probe_dreq) instead of guessed.
 */
#define MAI_DMA_DREQ       10u
#define MAI_DMA_DREQ_D0    12u
/* dma40 channel 6 = register-file base + 6*0x100 = 0x10_00010600 (dma@10600) */
#define MAI_DMA_CHANNEL    6u

/*
 * DREQ lines to try at runtime, most likely first. With the FIFO empty the
 * enabled MAI asserts its request line; a channel armed with the right PER_MAP
 * shows CS bit3 (DREQ) set or walks its control block past CB0.
 */
static const uint32_t _mai_dreq_candidates[] = { MAI_DMA_DREQ_D0, MAI_DMA_DREQ };
#define MAI_DREQ_CANDIDATE_COUNT \
        (sizeof(_mai_dreq_candidates) / sizeof(_mai_dreq_candidates[0]))

/*
 * Legacy-style info for a polled cyclic mem-to-peripheral transfer into the MAI
 * FIFO: WAIT_RESP | PER_MAP(dreq) | S_WIDTH | D_DREQ | S_INC. S_WIDTH issues
 * 128-bit reads from the (16-byte aligned) slot buffers while the destination
 * stays a 32-bit MAI FIFO port write, matching Circle's SetupCyclicIOWrite().
 */
#define MAI_DMA_INFO(dreq) (BCM2835_DMA_WAIT_RESP | BCM2835_DMA_PER_MAP(dreq) | \
        BCM2835_DMA_S_WIDTH | BCM2835_DMA_D_DREQ | BCM2835_DMA_S_INC)

/*
 * CS that arms a dma40 channel: bcm2835_dma_start_desc() writes ACTIVE | PROT
 * (supervisor) and the 40-bit path touches no global DMA_ENABLE register. QOS
 * stays 0 like Linux; raise DMA40_QOS/PANIC_QOS only if the FIFO ever starves.
 */
#define DMA40_CS_START     (DMA40_ACTIVE | DMA40_PROT)

/* a 40-bit channel is never "lite", so one control block spans up to 1 GB */
#define MAX_DMA_LEN        0x40000000u

/* the 32-byte dma40 control block (struct bcm2711_dma40_scb in bcm2835-dma.c) */
typedef struct __attribute__((packed)) {
    uint32_t ti;
    uint32_t src;
    uint32_t srci;
    uint32_t dst;
    uint32_t dsti;
    uint32_t len;
    uint32_t next_cb;
    uint32_t rsvd;
} dma40_scb_t;

typedef char dma40_scb_must_be_32_bytes[(sizeof(dma40_scb_t) == 32) ? 1 : -1];

/* legacy info -> dma40 TI (bcm2835-dma.c to_bcm2711_ti) */
static inline uint32_t to_bcm2711_ti(uint32_t info) {
    return ((info & BCM2835_DMA_INT_EN) ? DMA40_INTEN : 0u) |
            ((info & BCM2835_DMA_WAIT_RESP) ? DMA40_WAIT_RESP : 0u) |
            ((info & BCM2835_DMA_S_DREQ) ? (DMA40_S_DREQ | DMA40_WAIT_RD_RESP) : 0u) |
            ((info & BCM2835_DMA_D_DREQ) ? DMA40_D_DREQ : 0u) |
            DMA40_PER_MAP((info >> 16) & 0x1f);
}
/* legacy info -> dma40 SRCI (bcm2835-dma.c to_bcm2711_srci) */
static inline uint32_t to_bcm2711_srci(uint32_t info) {
    return ((info & BCM2835_DMA_S_INC) ? DMA40_INC : 0u) |
            ((info & BCM2835_DMA_S_WIDTH) ? DMA40_SIZE_128 : 0u) |
            DMA40_BURST_LEN(BCM2835_DMA_GET_BURST_LENGTH(info));
}
/* legacy info -> dma40 DSTI (bcm2835-dma.c to_bcm2711_dsti) */
static inline uint32_t to_bcm2711_dsti(uint32_t info) {
    return ((info & BCM2835_DMA_D_INC) ? DMA40_INC : 0u) |
            ((info & BCM2835_DMA_D_WIDTH) ? DMA40_SIZE_128 : 0u) |
            DMA40_BURST_LEN(BCM2835_DMA_GET_BURST_LENGTH(info));
}

/* dma40 fetches control blocks at addr>>5, so every SCB must be 32-byte aligned */
static inline uint32_t to_40bit_cbaddr(uint64_t addr) {
    return (uint32_t)(addr >> 5);
}

#define lower_32_bits(x) ((uint32_t)((uint64_t)(x) & 0xffffffffULL))
#define upper_32_bits(x) ((uint32_t)(((uint64_t)(x) >> 32) & 0xffffffffULL))

/* ------------------------------------------- dma40 bus address translation */
/*
 * The "axi" dma-ranges are identity, so a dma40 bus address IS the physical
 * address. RAM buffers keep the raw dma_phy_addr() value (dma_alloc() returns
 * memory below 1 GB, so srci's upper bits stay 0) and the MAI FIFO write port
 * is its full 40-bit physical address, whose upper 0x10 lands in dsti.
 */
#define HDMI_MAI_DATA_PHYS  0x107c72001cULL

/* control blocks are 32 bytes, so the ring keeps a 32-byte aligned address */
#define DMA40_CB_ALIGN      32u

/* --------------------------------------------------------------- state */

static uint8_t _ready;
static uint32_t _rate;
static uint32_t _channels;
static uint32_t _pixel_clock;      /* Hz, read from firmware in start() */
static int _hvs_step_d0 = -1;
static uint32_t _dreq;             /* DREQ line confirmed by the runtime probe */

static ewokos_addr_t _dma_base;    /* dma40 channel 6 register window */
static uint32_t _dma_max_frame;    /* MAX_DMA_LEN (a 40-bit channel is never lite) */

static ewokos_addr_t _ring_raw;    /* dma_alloc() handle */
static dma40_scb_t* _cb;           /* control block array (virtual) */
static uint64_t _cb_bus;           /* control block array bus addr (== phys) */
static uint32_t* _slots_virt;      /* sample buffers (virtual) */
static uint64_t _slots_bus;        /* sample buffers bus addr (== phys) */
static uint32_t _slot_count;
static uint32_t _slot_frames;
static bool _running;

/* ------------------------------------------------------------ accessors */

static inline void hdmi_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_HDMI_OFF + off, val);
}
static inline uint32_t hdmi_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_HDMI_OFF + off);
}
static inline void hd_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_HD_OFF + off, val);
}
static inline uint32_t hd_read(uint32_t off) {
    return get32(_mmio_base + PI5_HDMI0_HD_OFF + off);
}
static inline void ram_write(uint32_t off, uint32_t val) {
    put32(_mmio_base + PI5_HDMI0_RAM_OFF + off, val);
}
static inline uint32_t hvs_read(uint32_t off) {
    return get32(_mmio_base + PI5_HVS_OFF + off);
}
static inline uint32_t dma_get32(uint32_t off) { return get32(_dma_base + off); }
static inline void dma_put32(uint32_t off, uint32_t v) { put32(_dma_base + off, v); }

/* vc4_hdmi.c: the D0 step of the vc6 core is detected through the HVS id */
static int hvs_is_step_d0(void) {
    uint32_t hvs_id;
    if (_hvs_step_d0 >= 0)
        return _hvs_step_d0;
    hvs_id = hvs_read(SCALER6D0_HVS_ID);
    _hvs_step_d0 = (hvs_id != 0U && hvs_id != 0xffffffffU) ? 1 : 0;
    return _hvs_step_d0;
}

/* D0 remaps the HDMI audio DREQ from 10 to 12 (see MAI_DMA_DREQ_D0) */
static uint32_t mai_dma_dreq(void) {
    /* the runtime probe overrides the HVS-stepping guess once it has run */
    if (_dreq != 0)
        return _dreq;
    return hvs_is_step_d0() ? MAI_DMA_DREQ_D0 : MAI_DMA_DREQ;
}

/*
 * Open the vc6 HDMI audio clock gate before any MAI programming, mirroring
 * clk_prepare_enable(vc4_hdmi->audio_clock) in vc4_hdmi_runtime_resume(). The
 * read-modify-write clears only bit 3 (hdmi0-108MHz), leaving bit 4 (hdmi1) and
 * the other DVP config bits untouched.
 */
static void hdmi_audio_clock_enable(void) {
    ewokos_addr_t misc =
            _mmio_base + PI5_HDMI0_DVP_CLK_OFF + DVP_HT_RPI_MISC_CONFIG;
    put32(misc, get32(misc) & ~DVP_MISC_HDMI0_108MHZ_GATE);
}

/* -------------------------------------------------- native rate table */

const uint32_t hdmi_audio_native_rates[] = {
    32000u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u
};
const uint32_t hdmi_audio_native_rate_count =
        sizeof(hdmi_audio_native_rates) / sizeof(hdmi_audio_native_rates[0]);

/* sample_rate_to_mai_fmt() (vc4_hdmi.c) */
static uint32_t sample_rate_to_mai_fmt(uint32_t rate) {
    switch (rate) {
    case 32000:  return 7u;
    case 44100:  return 8u;
    case 48000:  return 9u;
    case 88200:  return 11u;
    case 96000:  return 12u;
    case 176400: return 14u;
    case 192000: return 15u;
    default:     return 0u;   /* NOT_INDICATED */
    }
}

/* ------------------------------------------ rational best approximation */
/*
 * rational_best_approximation() from lib/math/rational.c, the helper Linux and
 * Circle both use to split the 108 MHz MAI clock into N / (M + 1). Continued
 * fraction (Euclidean) convergents; when a convergent would exceed a bound the
 * closest semi-convergent is returned instead.
 */
static void rational_best_approximation(uint64_t given_numerator,
        uint64_t given_denominator, uint64_t max_numerator,
        uint64_t max_denominator, uint64_t* result_n, uint64_t* result_d) {
    uint64_t n = given_numerator;
    uint64_t d = given_denominator;
    uint64_t n0 = 0, d0 = 1;   /* two prior convergents */
    uint64_t n1 = 1, d1 = 0;   /* previous convergent */

    for (;;) {
        uint64_t dp, a, n2, d2;
        if (d == 0)
            break;
        dp = d;
        a = n / d;
        d = n % d;
        n = dp;

        n2 = n0 + a * n1;      /* current convergent */
        d2 = d0 + a * d1;

        if (n2 > max_numerator || d2 > max_denominator) {
            uint64_t t = (max_numerator - n0) / n1;
            uint64_t td = (max_denominator - d0) / d1;
            if (td < t)
                t = td;
            /* keep the semi-convergent only when it is closer */
            if (2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
                n1 = n0 + t * n1;
                d1 = d0 + t * d1;
            }
            break;
        }
        n0 = n1; n1 = n2;
        d0 = d1; d1 = d2;
    }
    *result_n = n1;
    *result_d = d1;
}

/* even parity fold: 1 when an odd number of bits are set */
static inline uint32_t parity32(uint32_t w) {
    w ^= w >> 16;
    w ^= w >> 8;
    w ^= w >> 4;
    w ^= w >> 2;
    w ^= w >> 1;
    return w & 1u;
}

/* ------------------------------------------------ firmware clock query */

typedef struct {
    uint32_t buf_size;
    uint32_t code;
    struct {
        uint32_t tag;
        uint32_t val_buf_size;
        uint32_t val_len;
        uint32_t clock_id;
        uint32_t rate_hz;
    } tag;
    uint32_t end_tag;
} __attribute__((packed)) fw_get_clock_req_t;

/* mirrors native_hdmi.c firmware_get_clock(): returns Hz, or 0 on failure */
static uint32_t firmware_get_clock(uint32_t clock_id) {
    fw_get_clock_req_t* req;
    ewokos_addr_t vaddr;
    mail_message_t msg;
    uint32_t rate = 0;

    vaddr = dma_alloc(0, sizeof(*req));
    if (vaddr == 0)
        return 0;

    req = (fw_get_clock_req_t*)(uintptr_t)vaddr;
    memset(req, 0, sizeof(*req));
    req->buf_size = sizeof(*req);
    req->tag.tag = RPI_FIRMWARE_GET_CLOCK_RATE;
    req->tag.val_buf_size = 8;
    req->tag.val_len = 4;
    req->tag.clock_id = clock_id;

    memset(&msg, 0, sizeof(msg));
    msg.data = (((uint32_t)dma_phy_addr(0, vaddr)) | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;

    if (bcm2712_mailbox_call_timeout(&msg, 0) == 0 &&
            (req->code & 0x80000000u) != 0 &&
            (req->tag.val_len & 0x80000000u) != 0) {
        rate = req->tag.rate_hz;
    }

    dma_free(0, vaddr);
    return rate;
}

/* --------------------------------------------------------- IEC958 word */

uint32_t hdmi_audio_iec958_subframe(int32_t sample24, uint32_t subframe_idx) {
    /* 24-bit two's-complement sample left-justified into bits [27:4] */
    uint32_t w = ((uint32_t)sample24 & 0x00FFFFFFu) << 4;
    /*
     * Basic PCM: validity (28), user (29) and channel-status (30) stay clear,
     * so the even-parity bit (31) is just the fold of the sample bits. The
     * preamble is added afterwards because bits [3:0] are not parity covered.
     */
    if (parity32(w))
        w |= 0x80000000u;
    /*
     * Both subframes of block frame 0 (LEFT then RIGHT, subframe_idx < 2)
     * carry the "B" preamble 0x8, which is exactly the value programmed into
     * AUDIO_PACKET_CONFIG.B_FRAME_IDENTIFIER so the MAI can find block starts.
     */
    if ((subframe_idx >> 1) == 0u)
        w |= 0x8u;
    return w;
}

/* -------------------------------------------------------------- MAI */

/*
 * vc4_hdmi_audio_reset() / Circle RunHDMI(): assert reset+flush and latch-clear
 * the delayed/error flags in ONE write, leaving the MAI disabled. It is enabled
 * only at the very end of hdmi_audio_start(), after the DMA is armed.
 */
static void mai_reset(void) {
    hd_write(HDMI_MAI_CTL,
            VC4_HD_MAI_CTL_RESET | VC4_HD_MAI_CTL_FLUSH | VC4_HD_MAI_CTL_DLATE |
            VC4_HD_MAI_CTL_ERRORE | VC4_HD_MAI_CTL_ERRORF);
}

/* vc4_hdmi_audio_set_mai_clock(): MAI_SMP = N / (M+1) of the 108 MHz clock */
static void set_mai_clock(uint32_t rate) {
    uint64_t n = 0, m = 0;
    rational_best_approximation(HDMI_AUDIO_CLOCK_HZ, rate,
            VC4_HD_MAI_SMP_N_MASK >> VC4_SHIFT(VC4_HD_MAI_SMP_N_MASK),
            (VC4_HD_MAI_SMP_M_MASK >> VC4_SHIFT(VC4_HD_MAI_SMP_M_MASK)) + 1,
            &n, &m);
    hd_write(HDMI_MAI_SMP,
            VC4_SET_FIELD(n, VC4_HD_MAI_SMP_N_MASK) |
            VC4_SET_FIELD(m - 1, VC4_HD_MAI_SMP_M_MASK));
}

/* vc4_hdmi_set_n_cts(): clock regeneration N / CTS from the real pixel clock */
static void set_n_cts(uint32_t rate) {
    uint32_t n;
    uint64_t tmp;
    uint32_t cts;

    /*
     * N is NOT 128*rate/1000. The 44.1 kHz family must use the HDMI-spec N
     * values (6272 / 12544 / 25088) so that CTS = pixel_clock*N/(128*fs)
     * stays integral at the standard pixel clocks -- this is exactly the
     * switch() in Linux vc4_hdmi_set_n_cts(). The old formula computed 5644
     * for 44100 (instead of 6272), so a 44.1 kHz stream drove the sink with a
     * wrong regenerated audio clock and stayed silent, while 48 kHz (whose
     * 128*48 == 6144 already matches the spec) played fine. That is why
     * nesemu (44100) had no sound but wavplayer (48000) did.
     */
    switch (rate) {
    case 32000:  n = 4096u;  break;
    case 44100:  n = 6272u;  break;
    case 48000:  n = 6144u;  break;
    case 88200:  n = 12544u; break;
    case 96000:  n = 12288u; break;
    case 176400: n = 25088u; break;
    case 192000: n = 24576u; break;
    default:     n = 128u * rate / 1000u; break;
    }

    tmp = (uint64_t)_pixel_clock * n;
    tmp /= (uint64_t)(128u * rate);
    cts = (uint32_t)tmp;

    hdmi_write(HDMI_CRP_CFG,
            VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN |
            VC4_SET_FIELD(n, VC4_HDMI_CRP_CFG_N_MASK));
    /* both CTS slots get the same value; the period fields alternate them */
    hdmi_write(HDMI_CTS_0, cts);
    hdmi_write(HDMI_CTS_1, cts);
}

/* vc4_hdmi_audio_prepare() MAI threshold step: the D0 core uses wider fields */
static void mai_set_threshold(void) {
    if (hvs_is_step_d0())
        hd_write(HDMI_MAI_THR,
                VC4_SET_FIELD(0x10, VC4_D0_HD_MAI_THR_PANICHIGH_MASK) |
                VC4_SET_FIELD(0x10, VC4_D0_HD_MAI_THR_PANICLOW_MASK) |
                VC4_SET_FIELD(0x1c, VC4_D0_HD_MAI_THR_DREQHIGH_MASK) |
                VC4_SET_FIELD(0x1c, VC4_D0_HD_MAI_THR_DREQLOW_MASK));
    else
        hd_write(HDMI_MAI_THR,
                VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_PANICHIGH_MASK) |
                VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_PANICLOW_MASK) |
                VC4_SET_FIELD(0x1c, VC4_HD_MAI_THR_DREQHIGH_MASK) |
                VC4_SET_FIELD(0x1c, VC4_HD_MAI_THR_DREQLOW_MASK));
}

/* vc5_hdmi_channel_map(): channel i is placed in nibble i */
static uint32_t mai_channel_map(uint32_t channel_mask) {
    uint32_t map = 0;
    for (uint32_t i = 0; i < 8u; i++)
        if (channel_mask & BIT(i))
            map |= i << (4 * i);
    return map;
}

/* wait up to ~100 ms for a RAM_PACKET_STATUS bit to reach `set` */
static int ram_packet_wait(uint32_t bit, bool set) {
    for (int i = 0; i < 100; i++) {
        uint32_t st = hdmi_read(HDMI_RAM_PACKET_STATUS) & bit;
        if (set ? (st != 0u) : (st == 0u))
            return 0;
        usleep(1000);
    }
    return -1;
}

/*
 * vc4_hdmi_write_infoframe() for a stereo basic-PCM CEA audio infoframe.
 * The packet RAM engine must be enabled first (native_hdmi.c leaves it off),
 * then the packet is stopped, rewritten and re-enabled.
 */
static int set_audio_infoframe(void) {
    uint32_t packet_reg =
            HDMI_RAM_PACKET_START + VC4_HDMI_PACKET_STRIDE * HDMI_AUDIO_PACKET_ID;
    uint32_t packet_reg_next = packet_reg + VC4_HDMI_PACKET_STRIDE;
    uint32_t cfg = hdmi_read(HDMI_RAM_PACKET_CONFIG);

    /* an all-ones read means the HDMI block is not clocked (no display up) */
    if (cfg == 0xffffffffu)
        return HDMI_AUDIO_ERR_NO_DISPLAY;

    if (!(cfg & VC4_HDMI_RAM_PACKET_ENABLE))
        hdmi_write(HDMI_RAM_PACKET_CONFIG, cfg | VC4_HDMI_RAM_PACKET_ENABLE);

    /* stop this packet and let the RAM engine release it */
    hdmi_write(HDMI_RAM_PACKET_CONFIG,
            hdmi_read(HDMI_RAM_PACKET_CONFIG) & ~BIT(HDMI_AUDIO_PACKET_ID));
    ram_packet_wait(BIT(HDMI_AUDIO_PACKET_ID), false);

    /*
     * CEA audio infoframe: type 0x84, version 1, length 10, stereo PCM.
     * DB0 = 1 (2 channels); every other data byte is "refer to stream".
     * checksum = 0x100 - ((0x84 + 0x01 + 0x0A + 0x01) & 0xFF) = 0x70.
     * Words pack the buffer little-endian, 3 bytes then 4 bytes per pair.
     */
    ram_write(packet_reg + 0x00, 0x000A0184u);   /* type | ver<<8 | len<<16 */
    ram_write(packet_reg + 0x04, 0x00000170u);   /* checksum | DB0<<8 */
    ram_write(packet_reg + 0x08, 0x00000000u);
    ram_write(packet_reg + 0x0c, 0x00000000u);
    /* clear the remainder so it is not counted into the checksum */
    for (uint32_t off = packet_reg + 0x10; off < packet_reg_next; off += 4)
        ram_write(off, 0);

    /* enable the audio packet and wait for the engine to pick it up */
    hdmi_write(HDMI_RAM_PACKET_CONFIG,
            hdmi_read(HDMI_RAM_PACKET_CONFIG) | BIT(HDMI_AUDIO_PACKET_ID));
    ram_packet_wait(BIT(HDMI_AUDIO_PACKET_ID), true);
    return HDMI_AUDIO_ERR_NONE;
}

/* --------------------------------------------------------------- dma40 */

/* bcm2835_dma_abort() for a 40-bit channel */
static void dma40_abort(void) {
    uint32_t timeout = 100000u;
    /* a zero CB means the channel is already idle (ACTIVE is not reliable) */
    if (dma_get32(DMA40_CB) == 0)
        return;
    /* pause the channel, then let outstanding bus transactions drain */
    dma_put32(DMA40_CS, dma_get32(DMA40_CS) & ~DMA40_ACTIVE);
    while ((dma_get32(DMA40_CS) & DMA40_TRANSACTIONS) && --timeout)
        ;   /* spin: bounded so a stuck peripheral cannot hang the driver */
    dma_put32(DMA40_CS, DMA40_PROT);
    dma_put32(DMA40_DEBUG, dma_get32(DMA40_DEBUG) | DMA40_DEBUG_RESET);
}

/*
 * Rewrite every SCB's TI with a candidate DREQ. Only TI carries PER_MAP, so
 * src/srci/dst/dsti/len/next_cb are left intact. Must only be called while the
 * channel is stopped, or the engine could fetch a half-written control block.
 */
static void ring_set_dreq(uint32_t dreq) {
    uint32_t ti = to_bcm2711_ti(MAI_DMA_INFO(dreq));
    for (uint32_t i = 0; i < _slot_count; i++)
        _cb[i].ti = ti;
    hdmi_dmb();
}

/*
 * bcm2835_dma_start_desc() for a 40-bit channel: point CB at the first control
 * block (addr>>5) and set ACTIVE|PROT. The 40-bit path has no global DMA_ENABLE
 * register to poke -- writing CS is what starts the engine walking the chain.
 */
static void dma40_arm_start(void) {
    dma40_abort();
    hdmi_dmb();
    dma_put32(DMA40_CB, to_40bit_cbaddr(_cb_bus));
    dma_put32(DMA40_CS, DMA40_CS_START);
    hdmi_dmb();
}

/*
 * Disable the MAI and drain its FIFO to EMPTY. The DREQ threshold is 0x1c
 * frames, so any residue left by a previous run keeps the MAI from asserting
 * its request line; a channel armed afterwards then parks in CS bit6
 * (DREQ_PAUSED) with a frozen CONBLK and a line that is really wired looks
 * dead. Flushing first makes the next enable raise DREQ deterministically.
 */
static void mai_disable_flush(void) {
    uint32_t drain = 100000u;
    hd_write(HDMI_MAI_CTL,
            VC4_HD_MAI_CTL_FLUSH | VC4_HD_MAI_CTL_DLATE |
            VC4_HD_MAI_CTL_ERRORE | VC4_HD_MAI_CTL_ERRORF);
    hdmi_dmb();
    while (!(hd_read(HDMI_MAI_CTL) & VC4_HD_MAI_CTL_EMPTY) && --drain)
        ;   /* bounded spin: a stuck FIFO must not hang the driver */
    /* drop FLUSH; the MAI stays disabled over a provably empty FIFO */
    hd_write(HDMI_MAI_CTL,
            VC4_HD_MAI_CTL_DLATE | VC4_HD_MAI_CTL_ERRORE |
            VC4_HD_MAI_CTL_ERRORF);
    hdmi_dmb();
}

/*
 * Discover which DREQ line the MAI actually drives on dma40. The MAI has to be
 * enabled to assert its request line, but enabling it on an empty FIFO before
 * the DMA is live underflows and latches DLATE (MAI_CTL bit15); once latched the
 * MAI stops issuing DREQ and every candidate reads a frozen CONBLK (start -6).
 * So each candidate is tried in the proven Circle order: arm the dma40 channel
 * first, clear any DLATE latched by the previous candidate, then enable the MAI
 * so the FIFO is fed the instant it is clocked. A line is wired if CS bit3
 * (DREQ) reads set or CB has walked past CB0. Returns the winning DREQ (also
 * stored in _dreq), or 0 if dma40 cannot service the MAI at all.
 */
static uint32_t mai_probe_dreq(void) {
    uint32_t cb0 = to_40bit_cbaddr(_cb_bus);
    uint32_t mai_run =
            VC4_SET_FIELD(_channels, VC4_HD_MAI_CTL_CHNUM_MASK) |
            VC4_HD_MAI_CTL_WHOLSMP |
            VC4_HD_MAI_CTL_CHALIGN |
            VC4_HD_MAI_CTL_ENABLE;
    for (uint32_t i = 0; i < MAI_DREQ_CANDIDATE_COUNT; i++) {
        uint32_t dreq = _mai_dreq_candidates[i];
        dma40_abort();
        ring_set_dreq(dreq);
        /* disable + flush so the MAI provably starts from an empty FIFO */
        mai_disable_flush();
        /* Circle order: DMA armed first, then the MAI is enabled into the empty
         * FIFO so it raises DREQ the instant it is clocked */
        dma40_arm_start();
        hd_write(HDMI_MAI_CTL, mai_run);
        hdmi_dmb();
        usleep(3000);
        uint32_t cs = dma_get32(DMA40_CS);
        uint32_t cb = dma_get32(DMA40_CB);
        uint32_t ctl = hd_read(HDMI_MAI_CTL);
        /*
         * mai_disable_flush() left the FIFO EMPTY, so a cleared EMPTY bit is
         * direct proof the DMA delivered samples on this line. Trust it: at
         * 44.1 kHz a 3 ms probe moves only ~1 KB, far short of one 16 KB slot,
         * so CB is still CB0, and the FIFO crossing DREQLOW drops DREQ before
         * CS is sampled. The old (DREQ || CB!=CB0) test therefore missed a line
         * that actually works and the driver retried for seconds until DREQ
         * happened to read high -- exactly the long silence before first audio.
         */
        if ((cs & DMA40_DREQ) || cb != cb0 ||
                !(ctl & VC4_HD_MAI_CTL_EMPTY)) {
            _dreq = dreq;
            return dreq;   /* MAI enabled and DMA walking: leave it running */
        }
    }
    return 0;
}

/* ------------------------------------------------------------- windows */

static int audio_map_windows(void) {
    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;

    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
        slog("hdmi-audio: main MMIO map failed\n");
        return HDMI_AUDIO_ERR_MAP;
    }

    /*
     * One 64 KB window covers the whole DMA register file (dma32 @ +0x000 and
     * dma40 @ +0x600 are contiguous slices of it), so dma40 channel 6 at
     * 0x10_00010600 is reached at dma_vbase + 6*0x100 with no extra mapping and
     * no kernel-whitelist change.
     */
    ewokos_addr_t dma_vbase = _mmio_base + PI5_DMA32_WIN_OFF;
    if (syscall3(SYS_MEM_MAP, dma_vbase, PI5_DMA32_PHY, PI5_DMA32_WIN_SIZE)
            != dma_vbase) {
        slog("hdmi-audio: dma window map failed\n");
        return HDMI_AUDIO_ERR_MAP;
    }

    _dma_base = dma_vbase + MAI_DMA_CHANNEL * DMA_CHAN_STRIDE;
    return HDMI_AUDIO_ERR_NONE;
}

/* -------------------------------------------------------------- public */

int hdmi_audio_init(uint32_t flags) {
    (void)flags;
    if (_ready)
        return HDMI_AUDIO_ERR_NONE;

    int ret = audio_map_windows();
    if (ret != HDMI_AUDIO_ERR_NONE)
        return ret;

    /* dma40 channel 6 is a bulk (non-lite) channel: one control block spans 1 GB */
    _dma_max_frame = MAX_DMA_LEN;

    /* the MAI needs its own gated 108 MHz clock running before it is reset */
    hdmi_audio_clock_enable();
    mai_reset();

    _ready = 1;
    return HDMI_AUDIO_ERR_NONE;
}

int hdmi_audio_config(uint32_t rate, uint32_t channels) {
    if (!_ready)
        return HDMI_AUDIO_ERR_STATE;
    if (_running)
        return HDMI_AUDIO_ERR_STATE;
    if (!hdmi_audio_rate_supported(rate))
        return HDMI_AUDIO_ERR_PARAM;
    if (channels != HDMI_AUDIO_CHANNELS)
        return HDMI_AUDIO_ERR_PARAM;
    _rate = rate;
    _channels = channels;
    return HDMI_AUDIO_ERR_NONE;
}

uint32_t hdmi_audio_rate(void) { return _rate; }

bool hdmi_audio_rate_supported(uint32_t rate) {
    for (uint32_t i = 0; i < hdmi_audio_native_rate_count; i++)
        if (hdmi_audio_native_rates[i] == rate)
            return true;
    return false;
}

uint32_t hdmi_audio_nearest_rate(uint32_t rate) {
    uint32_t best = hdmi_audio_native_rates[0];
    for (uint32_t i = 0; i < hdmi_audio_native_rate_count; i++) {
        uint32_t r = hdmi_audio_native_rates[i];
        uint32_t dbest = best > rate ? best - rate : rate - best;
        uint32_t dr = r > rate ? r - rate : rate - r;
        /* strictly closer wins; a tie rounds up to the higher rate */
        if (dr < dbest || (dr == dbest && r > best))
            best = r;
    }
    return best;
}

int hdmi_audio_setup_ring(uint32_t slots, uint32_t slot_frames) {
    if (!_ready)
        return HDMI_AUDIO_ERR_STATE;
    if (_running)
        return HDMI_AUDIO_ERR_STATE;
    /* the guard band needs room, and the ring has to be worth polling */
    if (slots < (2u * HDMI_AUDIO_GUARD_SLOTS + 2u) || slots > 64u)
        return HDMI_AUDIO_ERR_PARAM;
    /* a multiple of 16 frames keeps every slot buffer 128-byte aligned */
    if (slot_frames < 16u || slot_frames > 0x10000u || (slot_frames & 15u) != 0u)
        return HDMI_AUDIO_ERR_PARAM;

    uint32_t slot_bytes = slot_frames * HDMI_AUDIO_FRAME_WORDS * 4u;
    /* one control block per slot, so a slot must fit a single CB */
    if (slot_bytes > _dma_max_frame)
        return HDMI_AUDIO_ERR_PARAM;

    if (_cb != NULL)
        hdmi_audio_teardown_ring();

    uint32_t cb_bytes = slots * (uint32_t)sizeof(dma40_scb_t);
    uint32_t buf_bytes = slots * slot_bytes;
    uint32_t total = cb_bytes + buf_bytes;

    /* over-allocate one alignment unit; dma_alloc() only guarantees a page */
    ewokos_addr_t raw = dma_alloc(0, total + DMA40_CB_ALIGN);
    if (raw == 0) {
        slog("hdmi-audio: ring dma_alloc(%u) failed\n", total + DMA40_CB_ALIGN);
        return HDMI_AUDIO_ERR_DMA_MEM;
    }
    ewokos_addr_t phy = dma_phy_addr(0, raw);
    if (phy == 0) {
        slog("hdmi-audio: ring dma_phy_addr failed\n");
        dma_free(0, raw);
        return HDMI_AUDIO_ERR_DMA_MEM;
    }

    /* SCBs are fetched at addr>>5, so the chain must start 32-byte aligned */
    uint32_t pad = (uint32_t)((DMA40_CB_ALIGN - (phy & (DMA40_CB_ALIGN - 1u)))
            & (DMA40_CB_ALIGN - 1u));
    _ring_raw = raw;
    _cb = (dma40_scb_t*)(raw + pad);
    /* axi dma-ranges are identity: the bus address IS the raw physical address */
    _cb_bus = (uint64_t)(phy + pad);
    _slots_virt = (uint32_t*)((uint8_t*)_cb + cb_bytes);
    _slots_bus = _cb_bus + cb_bytes;
    _slot_count = slots;
    _slot_frames = slot_frames;

    memset(_cb, 0, cb_bytes);
    memset(_slots_virt, 0, buf_bytes);

    /*
     * Build the closed cyclic chain: one SCB per slot, memory -> MAI FIFO.
     * src walks the sample buffers (SRCI.INC), dst is the fixed FIFO port, and
     * the last SCB links back to the first so the engine never stops. Each 40-bit
     * address splits across src/srci and dst/dsti (upper 8 bits in the *I word)
     * and next_cb holds the next SCB's physical address >> 5.
     */
    uint32_t info = MAI_DMA_INFO(mai_dma_dreq());
    uint32_t ti = to_bcm2711_ti(info);
    uint32_t srci_attr = to_bcm2711_srci(info);
    uint32_t dsti_attr = to_bcm2711_dsti(info);
    for (uint32_t i = 0; i < slots; i++) {
        dma40_scb_t* scb = &_cb[i];
        uint64_t src = _slots_bus + (uint64_t)i * slot_bytes;
        uint64_t next_phys = _cb_bus +
                (uint64_t)(((i + 1u) % slots) * sizeof(dma40_scb_t));
        scb->ti = ti;
        scb->src = lower_32_bits(src);
        scb->srci = upper_32_bits(src) | srci_attr;
        scb->dst = lower_32_bits(HDMI_MAI_DATA_PHYS);
        scb->dsti = upper_32_bits(HDMI_MAI_DATA_PHYS) | dsti_attr;
        scb->len = slot_bytes;
        scb->next_cb = to_40bit_cbaddr(next_phys);
        scb->rsvd = 0;
    }
    hdmi_dmb();
    return HDMI_AUDIO_ERR_NONE;
}

void hdmi_audio_teardown_ring(void) {
    if (_running)
        hdmi_audio_stop();
    _cb = NULL;
    _cb_bus = 0;
    _slots_virt = NULL;
    _slots_bus = 0;
    _slot_count = 0;
    _slot_frames = 0;
    if (_ring_raw != 0) {
        dma_free(0, _ring_raw);
        _ring_raw = 0;
    }
}

int hdmi_audio_start(void) {
    if (!_ready)
        return HDMI_AUDIO_ERR_STATE;
    if (_cb == NULL || _slot_count == 0)
        return HDMI_AUDIO_ERR_STATE;
    if (_rate == 0)
        return HDMI_AUDIO_ERR_STATE;

    /* N/CTS is derived from the live pixel clock; no mode means no clock */
    _pixel_clock = firmware_get_clock(PI5_FW_CLK_HDMI0_PIXEL);
    if (_pixel_clock == 0) {
        slog("hdmi-audio: HDMI0 pixel clock unavailable\n");
        return HDMI_AUDIO_ERR_CLOCK;
    }

    /*
     * Program the whole MAI chain but leave it DISABLED, exactly like Circle's
     * RunHDMI() and vc4_hdmi_audio_prepare() minus the enable. The MAI must not
     * be turned on before the DMA is armed: an enabled-but-starved FIFO
     * underflows, latches DLATE/ERRORE and stops issuing its DREQ, so a channel
     * started afterwards parks forever in CS = ACTIVE|ISHELD (0x21) with a
     * frozen CONBLK_AD and no sample ever reaches the FIFO.
     */
    mai_reset();
    set_mai_clock(_rate);
    hd_write(HDMI_MAI_FMT,
            VC4_SET_FIELD(sample_rate_to_mai_fmt(_rate),
                    VC4_HD_MAI_FMT_SAMPLE_RATE_MASK) |
            VC4_SET_FIELD(VC4_HDMI_MAI_FORMAT_PCM,
                    VC4_HD_MAI_FMT_AUDIO_FORMAT_MASK));

    uint32_t channel_mask = (1u << _channels) - 1u;   /* 0x3 for stereo */
    uint32_t audio_packet_config =
            VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT |
            VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS |
            VC4_SET_FIELD(0x8, VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER_MASK) |
            VC4_SET_FIELD(channel_mask, VC4_HDMI_AUDIO_PACKET_CEA_MASK_MASK);

    mai_set_threshold();
    hdmi_write(HDMI_MAI_CONFIG,
            VC4_HDMI_MAI_CONFIG_BIT_REVERSE |
            VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE |
            VC4_SET_FIELD(channel_mask, VC4_HDMI_MAI_CHANNEL_MASK_MASK));
    hdmi_write(HDMI_MAI_CHANNEL_MAP, mai_channel_map(channel_mask));
    hdmi_write(HDMI_AUDIO_PACKET_CONFIG, audio_packet_config);
    set_n_cts(_rate);

    int ret = set_audio_infoframe();
    if (ret != HDMI_AUDIO_ERR_NONE)
        return ret;

    /*
     * The MAI request line is ambiguous across sources (dtsi DREQ 10 vs Circle's
     * D0 DREQ 12 vs 6.18's move to dma40), so probe it instead of guessing.
     * mai_probe_dreq() enables the MAI itself, per candidate, only AFTER that
     * candidate's dma40 channel is armed. Enabling it here first -- on an empty
     * FIFO with no DMA live -- underflows and latches DLATE, which stops the
     * DREQ and fails every probe (start -6).
     */
    if (mai_probe_dreq() == 0) {
        slog("hdmi-audio: no dma40 DREQ services the MAI (tried %u,%u) cs=%08x cb=%08x\n",
                MAI_DMA_DREQ_D0, MAI_DMA_DREQ,
                dma_get32(DMA40_CS), dma_get32(DMA40_CB));
        dma40_abort();
        return HDMI_AUDIO_ERR_STATE;
    }

    /*
     * Restart cleanly on the winning line with the proven Circle order: stop the
     * probe channel, disable the MAI and clear any latched error, re-arm the DMA
     * (SCBs already carry the winning PER_MAP), then enable the MAI last so the
     * empty FIFO is filled before any underflow can latch DLATE.
     */
    dma40_abort();
    /* flush the residue the probe left so this clean restart also begins on an
     * empty FIFO and pulls DREQ immediately on the enable below */
    mai_disable_flush();
    dma40_arm_start();

    if (!(dma_get32(DMA40_CS) & DMA40_ACTIVE)) {
        slog("hdmi-audio: dma40 channel %u refused to start cs=%08x\n",
                MAI_DMA_CHANNEL, dma_get32(DMA40_CS));
        return HDMI_AUDIO_ERR_STATE;
    }

    hd_write(HDMI_MAI_CTL,
            VC4_SET_FIELD(_channels, VC4_HD_MAI_CTL_CHNUM_MASK) |
            VC4_HD_MAI_CTL_WHOLSMP |
            VC4_HD_MAI_CTL_CHALIGN |
            VC4_HD_MAI_CTL_ENABLE);
    hdmi_dmb();

    _running = true;
    return HDMI_AUDIO_ERR_NONE;
}

void hdmi_audio_stop(void) {
    if (!_ready)
        return;
    dma40_abort();
    /* vc4_hdmi_audio_shutdown(): mute, stop the infoframe, then reset */
    hd_write(HDMI_MAI_CTL,
            VC4_HD_MAI_CTL_DLATE | VC4_HD_MAI_CTL_ERRORE | VC4_HD_MAI_CTL_ERRORF);
    hdmi_write(HDMI_RAM_PACKET_CONFIG,
            hdmi_read(HDMI_RAM_PACKET_CONFIG) & ~BIT(HDMI_AUDIO_PACKET_ID));
    mai_reset();
    _running = false;
}

bool hdmi_audio_running(void) {
    if (!_running || !_ready)
        return false;
    /* the ring is free-running, so a dropped ACTIVE bit means it died */
    return (dma_get32(DMA40_CS) & DMA40_ACTIVE) != 0u;
}

uint32_t hdmi_audio_slots(void) { return _slot_count; }
uint32_t hdmi_audio_slot_frames(void) { return _slot_frames; }

uint32_t* hdmi_audio_slot_buffer(uint32_t slot) {
    if (_slots_virt == NULL || slot >= _slot_count)
        return NULL;
    return _slots_virt + (size_t)slot * _slot_frames * HDMI_AUDIO_FRAME_WORDS;
}

int hdmi_audio_hw_slot(void) {
    if (!_ready || _cb == NULL || _slot_count == 0)
        return -1;
    if (!(dma_get32(DMA40_CS) & DMA40_ACTIVE))
        return -1;

    /*
     * The dma40 CB register always holds the physical address (>>5) of the
     * control block the engine is executing, and we build exactly one SCB per
     * slot, so the SCB index is the slot being drained into the FIFO. Shift the
     * readback back up to a byte address before comparing against _cb_bus.
     */
    uint64_t cb = (uint64_t)dma_get32(DMA40_CB) << 5;
    if (cb < _cb_bus)
        return -1;
    uint64_t off = cb - _cb_bus;
    if (off >= (uint64_t)_slot_count * sizeof(dma40_scb_t))
        return -1;
    if ((off % sizeof(dma40_scb_t)) != 0u)
        return -1;
    return (int)(off / sizeof(dma40_scb_t));
}

bool hdmi_audio_slot_writable(uint32_t slot) {
    if (_slot_count == 0 || slot >= _slot_count)
        return false;
    int hw = hdmi_audio_hw_slot();
    if (hw < 0)
        return false;
    /* forward distance from the slot the engine is reading */
    uint32_t d = (slot + _slot_count - (uint32_t)hw) % _slot_count;
    /*
     * d == 0 is in flight and the slots just behind it may already be latched;
     * anything at least GUARD slots ahead will not be fetched for several slot
     * periods (milliseconds), which is plenty of time to refill it.
     */
    return d > HDMI_AUDIO_GUARD_SLOTS &&
            d < (_slot_count - HDMI_AUDIO_GUARD_SLOTS);
}

void hdmi_audio_slot_commit(uint32_t slot) {
    if (_cb == NULL || slot >= _slot_count)
        return;
    /*
     * The engine never writes back into a bcm2835 control block, so the cyclic
     * chain stays valid for the whole run and there is nothing to re-arm. This
     * is only a store barrier ordering the sample writes before the next poll
     * of the engine position; kept for API symmetry with rp1_audio.
     */
    hdmi_dmb();
}
