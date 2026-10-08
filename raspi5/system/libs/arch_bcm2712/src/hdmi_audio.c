/*
 * HDMI (VC4 MAI) + legacy dma32 cyclic ring playback backend for BCM2712
 * (Raspberry Pi 5). See hdmi_audio.h for the caller-facing model.
 *
 * Everything here is polled: no dma32 interrupt reaches user space, so the
 * channel is left walking a closed chain of bcm2835 control blocks forever and
 * its position is read back from SOURCE_AD.
 *
 * Register maps and programming sequences come from:
 *   drivers/gpu/drm/vc4/vc4_hdmi.c        MAI bring-up, N/CTS, audio infoframe
 *   drivers/gpu/drm/vc4/vc4_hdmi_regs.h   vc6_hdmi_hdmi0_fields[], MAI bitfields
 *   drivers/dma/bcm2835-dma.c             legacy dma32 engine (is_2712 path)
 *   arch/arm64/boot/dts/broadcom/bcm2712.dtsi  addresses, dma-ranges,
 *                                         hdmi0 dmas = <&dma32 10>
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
#define HDMI_MAI_DATA       0x001c   /* FIFO write port: the dma32 destination */
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
 * The vc6 MAI serial clock ("audio" clock) is a fixed 108 MHz (clk_108MHz in
 * bcm2712.dtsi); Circle hard-codes the same value. MAI_SMP divides it down to
 * the sample rate, so this never has to be queried.
 */
#define HDMI_AUDIO_CLOCK_HZ  108000000ULL

/* firmware mailbox clock query (native_hdmi.c uses the same tag/id) */
#define RPI_FIRMWARE_GET_CLOCK_RATE  0x00030002u
#define PI5_FW_CLK_HDMI0_PIXEL       9u

/* ------------------------------------------------- legacy dma32 engine */

/* per-channel register stride and registers (bcm2835-dma.c) */
#define DMA32_CHAN_STRIDE     0x100u
#define BCM2835_DMA_CS        0x00
#define BCM2835_DMA_ADDR      0x04   /* CONBLK_AD */
#define BCM2835_DMA_TI        0x08
#define BCM2835_DMA_SOURCE_AD 0x0c
#define BCM2835_DMA_DEST_AD   0x10
#define BCM2835_DMA_LEN       0x14
#define BCM2835_DMA_STRIDE    0x18
#define BCM2835_DMA_NEXTCB    0x1c
#define BCM2835_DMA_DEBUG     0x20

/* CS bits */
#define BCM2835_DMA_ACTIVE    BIT(0)
#define BCM2835_DMA_END       BIT(1)
#define BCM2835_DMA_DREQ      BIT(3)
#define BCM2835_DMA_ERR       BIT(8)
#define BCM2835_DMA_PRIORITY(x)      (((x) & 15) << 16)
#define BCM2835_DMA_PANIC_PRIORITY(x) (((x) & 15) << 20)
#define BCM2835_DMA_WAIT_FOR_WRITES  BIT(28)
#define BCM2835_DMA_DIS_DEBUG        BIT(29)
#define BCM2835_DMA_ABORT     BIT(30)
#define BCM2835_DMA_RESET     BIT(31)
#define BCM2835_DMA_CS_FLAGS(x) ((x) & (BCM2835_DMA_PRIORITY(15) | \
        BCM2835_DMA_PANIC_PRIORITY(15) | BCM2835_DMA_WAIT_FOR_WRITES | \
        BCM2835_DMA_DIS_DEBUG))

/* TI / control-block info bits */
#define BCM2835_DMA_INT_EN    BIT(0)
#define BCM2835_DMA_WAIT_RESP BIT(3)
#define BCM2835_DMA_D_INC     BIT(4)
#define BCM2835_DMA_D_DREQ    BIT(6)
#define BCM2835_DMA_S_INC     BIT(8)
#define BCM2835_DMA_S_DREQ    BIT(10)
#define BCM2835_DMA_PER_MAP(x) (((x) & 31) << 16)
#define BCM2835_DMA_NO_WIDE_BURSTS BIT(26)

#define BCM2835_DMA_DEBUG_LITE BIT(28)

/* MAI is peripheral request line 10 (bcm2712.dtsi: hdmi0 dmas = <&dma32 10>) */
#define MAI_DMA_DREQ     10u
/* channel 0 of dma32 (brcm,dma-channel-mask = <0x0035> = {0,2,4,5}) */
#define MAI_DMA_CHANNEL  0u

/*
 * info for a polled cyclic mem-to-peripheral transfer into the MAI FIFO.
 * bcm2835_dma_prep_dma_cyclic() builds exactly this for dreq 10:
 *   WAIT_RESP | PER_MAP(10) | D_DREQ | S_INC
 * The four "fake" width/burst request bits are clear for a plain dreq number,
 * and the slave path never adds NO_WIDE_BURSTS, so the value is constant.
 */
#define MAI_DMA_INFO (BCM2835_DMA_WAIT_RESP | BCM2835_DMA_PER_MAP(MAI_DMA_DREQ) | \
        BCM2835_DMA_D_DREQ | BCM2835_DMA_S_INC)

/* a lite channel caps one control block at 64K-4; a bulk channel at 1G */
#define MAX_LITE_DMA_LEN  (65536u - 4u)
#define MAX_DMA_LEN       0x40000000u

/* the 32-byte control block the engine fetches (struct bcm2835_dma_cb) */
typedef struct __attribute__((packed)) {
    uint32_t info;
    uint32_t src;
    uint32_t dst;
    uint32_t length;
    uint32_t stride;
    uint32_t next;
    uint32_t pad[2];
} bcm2835_dma_cb_t;

typedef char bcm2835_dma_cb_must_be_32_bytes[(sizeof(bcm2835_dma_cb_t) == 32) ? 1 : -1];

/* CONBLK_AD and CB.next hold a bus address shifted right by 5 (to_40bit_cbaddr) */
static inline uint32_t to_40bit_cbaddr(uint64_t addr) {
    return (uint32_t)(addr >> 5);
}

/* ------------------------------------------ dma32 bus address translation */
/*
 * bcm2712.dtsi dma-ranges for the legacy dma32 engine:
 *   RAM:        <0xc0000000 0x00 0x00000000 0x40000000>  bus = 0xc0000000 + phys
 *   peripheral: <0x7c000000 0x10 0x7c000000 0x04000000>  bus = phys - 0x10_00000000
 * The MAI FIFO is at phys 0x10_7c72001c, i.e. bus 0x7c72001c. dma_alloc()
 * returns memory below 1 GB, so every RAM bus address stays under 0x1_00000000
 * and the CB.stride upper-address bits are zero.
 */
#define DMA32_RAM_BUS_OFF  0xc0000000u
#define HDMI_MAI_DATA_BUS  0x7c72001cu

/* to_40bit_cbaddr() needs a 32-byte aligned bus address */
#define DMA32_CB_ALIGN     32u

/* --------------------------------------------------------------- state */

static uint8_t _ready;
static uint32_t _rate;
static uint32_t _channels;
static uint32_t _pixel_clock;      /* Hz, read from firmware in start() */
static int _hvs_step_d0 = -1;

static ewokos_addr_t _dma_base;    /* dma32 channel register window */
static uint32_t _dma_max_frame;    /* MAX_DMA_LEN or MAX_LITE_DMA_LEN */

static ewokos_addr_t _ring_raw;    /* dma_alloc() handle */
static bcm2835_dma_cb_t* _cb;      /* control block array (virtual) */
static uint64_t _cb_bus;           /* control block array (dma32 bus) */
static uint32_t* _slots_virt;      /* sample buffers (virtual) */
static uint64_t _slots_bus;        /* sample buffers (dma32 bus) */
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

/* vc4_hdmi_audio_reset(): single-shot reset, then clear the error/flush bits */
static void mai_reset(void) {
    hd_write(HDMI_MAI_CTL, VC4_HD_MAI_CTL_RESET);
    hd_write(HDMI_MAI_CTL, VC4_HD_MAI_CTL_ERRORF);
    hd_write(HDMI_MAI_CTL, VC4_HD_MAI_CTL_FLUSH);
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
    uint32_t n = 128u * rate / 1000u;
    uint64_t tmp = (uint64_t)_pixel_clock * n;
    uint32_t cts;
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
    if (ram_packet_wait(BIT(HDMI_AUDIO_PACKET_ID), false) != 0)
        klog("hdmi-audio: audio infoframe did not go idle\n");

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
    if (ram_packet_wait(BIT(HDMI_AUDIO_PACKET_ID), true) != 0)
        klog("hdmi-audio: audio infoframe did not start\n");
    return HDMI_AUDIO_ERR_NONE;
}

/* --------------------------------------------------------------- dma32 */

/* bcm2835_dma_terminate_all() for a non-40bit channel */
static void dma32_abort(void) {
    uint32_t timeout = 100000u;
    /* a zero CONBLK_AD means the channel is already idle */
    if (dma_get32(BCM2835_DMA_ADDR) == 0)
        return;
    dma_put32(BCM2835_DMA_NEXTCB, 0);
    dma_put32(BCM2835_DMA_CS,
            dma_get32(BCM2835_DMA_CS) | BCM2835_DMA_ABORT | BCM2835_DMA_ACTIVE);
    while ((dma_get32(BCM2835_DMA_CS) & BCM2835_DMA_ABORT) && --timeout)
        ;   /* spin: the abort completes within a few bus cycles */
    dma_put32(BCM2835_DMA_CS, dma_get32(BCM2835_DMA_CS) & ~BCM2835_DMA_ACTIVE);
    dma_put32(BCM2835_DMA_CS, BCM2835_DMA_RESET);
}

/* ------------------------------------------------------------- windows */

static int audio_map_windows(void) {
    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;

    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
        klog("hdmi-audio: main MMIO map failed\n");
        return HDMI_AUDIO_ERR_MAP;
    }

    ewokos_addr_t dma_vbase = _mmio_base + PI5_DMA32_WIN_OFF;
    if (syscall3(SYS_MEM_MAP, dma_vbase, PI5_DMA32_PHY, PI5_DMA32_WIN_SIZE)
            != dma_vbase) {
        klog("hdmi-audio: dma32 window map failed\n");
        return HDMI_AUDIO_ERR_MAP;
    }

    _dma_base = dma_vbase + MAI_DMA_CHANNEL * DMA32_CHAN_STRIDE;
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

    /* a lite channel would cap each control block; record the real limit */
    _dma_max_frame = (dma_get32(BCM2835_DMA_DEBUG) & BCM2835_DMA_DEBUG_LITE)
            ? MAX_LITE_DMA_LEN : MAX_DMA_LEN;

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

    uint32_t cb_bytes = slots * (uint32_t)sizeof(bcm2835_dma_cb_t);
    uint32_t buf_bytes = slots * slot_bytes;
    uint32_t total = cb_bytes + buf_bytes;

    /* over-allocate one alignment unit; dma_alloc() only guarantees a page */
    ewokos_addr_t raw = dma_alloc(0, total + DMA32_CB_ALIGN);
    if (raw == 0) {
        klog("hdmi-audio: ring dma_alloc(%u) failed\n", total + DMA32_CB_ALIGN);
        return HDMI_AUDIO_ERR_DMA_MEM;
    }
    ewokos_addr_t phy = dma_phy_addr(0, raw);
    if (phy == 0) {
        klog("hdmi-audio: ring dma_phy_addr failed\n");
        dma_free(0, raw);
        return HDMI_AUDIO_ERR_DMA_MEM;
    }

    /* the DMA window is mapped linearly, so one pad aligns both views */
    uint32_t pad = (uint32_t)((DMA32_CB_ALIGN - (phy & (DMA32_CB_ALIGN - 1u)))
            & (DMA32_CB_ALIGN - 1u));
    _ring_raw = raw;
    _cb = (bcm2835_dma_cb_t*)(raw + pad);
    _cb_bus = (uint64_t)(phy + pad) + DMA32_RAM_BUS_OFF;
    _slots_virt = (uint32_t*)((uint8_t*)_cb + cb_bytes);
    _slots_bus = _cb_bus + cb_bytes;
    _slot_count = slots;
    _slot_frames = slot_frames;

    memset(_cb, 0, cb_bytes);
    memset(_slots_virt, 0, buf_bytes);

    /*
     * Build the closed cyclic chain: one CB per slot, memory -> MAI FIFO.
     * src walks the sample buffers (S_INC), dst is the fixed FIFO port, and
     * the last CB links back to the first so the engine never stops. CB.next
     * and CONBLK_AD are bus addresses >> 5 (is_2712 encoding).
     */
    for (uint32_t i = 0; i < slots; i++) {
        bcm2835_dma_cb_t* cb = &_cb[i];
        uint64_t next_bus = _cb_bus +
                (uint64_t)(((i + 1u) % slots) * sizeof(bcm2835_dma_cb_t));
        cb->info = MAI_DMA_INFO;
        cb->src = (uint32_t)(_slots_bus + (uint64_t)i * slot_bytes);
        cb->dst = HDMI_MAI_DATA_BUS;
        cb->length = slot_bytes;
        cb->stride = 0;   /* upper 32 bits of src and dst are both zero */
        cb->next = to_40bit_cbaddr(next_bus);
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
        klog("hdmi-audio: HDMI0 pixel clock unavailable\n");
        return HDMI_AUDIO_ERR_CLOCK;
    }

    /* vc4_hdmi_audio_prepare(): reset, then program the whole MAI chain */
    mai_reset();
    set_mai_clock(_rate);
    hd_write(HDMI_MAI_CTL,
            VC4_SET_FIELD(_channels, VC4_HD_MAI_CTL_CHNUM_MASK) |
            VC4_HD_MAI_CTL_WHOLSMP |
            VC4_HD_MAI_CTL_CHALIGN |
            VC4_HD_MAI_CTL_ENABLE);
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

    /* prime the ring and let the engine walk it (bcm2835_dma_start_desc) */
    dma32_abort();
    hdmi_dmb();
    dma_put32(BCM2835_DMA_CS, BCM2835_DMA_RESET);
    dma_put32(BCM2835_DMA_ADDR, to_40bit_cbaddr(_cb_bus));
    dma_put32(BCM2835_DMA_CS,
            BCM2835_DMA_ACTIVE | BCM2835_DMA_CS_FLAGS(MAI_DMA_DREQ));
    hdmi_dmb();

    _running = (dma_get32(BCM2835_DMA_CS) & BCM2835_DMA_ACTIVE) != 0u;
    if (!_running) {
        klog("hdmi-audio: dma32 channel %u refused to start cs=%08x\n",
                MAI_DMA_CHANNEL, dma_get32(BCM2835_DMA_CS));
        return HDMI_AUDIO_ERR_STATE;
    }
    return HDMI_AUDIO_ERR_NONE;
}

void hdmi_audio_stop(void) {
    if (!_ready)
        return;
    dma32_abort();
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
    return (dma_get32(BCM2835_DMA_CS) & BCM2835_DMA_ACTIVE) != 0u;
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
    if (!(dma_get32(BCM2835_DMA_CS) & BCM2835_DMA_ACTIVE))
        return -1;

    /*
     * For a mem-to-dev transfer the engine keeps SOURCE_AD pointed at the byte
     * it is about to read; bcm2835_dma_tx_status() reads the same register.
     * Our bus addresses fit in 32 bits, so the read is the full address.
     */
    uint64_t pos = dma_get32(BCM2835_DMA_SOURCE_AD);
    if (pos < _slots_bus)
        return -1;
    uint64_t off = pos - _slots_bus;
    uint32_t slot_bytes = _slot_frames * HDMI_AUDIO_FRAME_WORDS * 4u;
    if (off >= (uint64_t)_slot_count * slot_bytes)
        return -1;
    return (int)(off / slot_bytes);
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
