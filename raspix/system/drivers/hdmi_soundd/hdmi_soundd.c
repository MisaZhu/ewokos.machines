/*
 * hdmi_soundd - HDMI audio (VC4 "MAI") sound card driver.
 *
 * Compatible with Raspberry Pi 4 (bcm2711 HDMI0), Pi 3 (bcm2837) and
 * Pi Zero 2 (bcm2837). It presents the exact same PCM client contract as
 * the analog soundd (CTRL_PCM_DEV_HW / PRPARE / BUF_AVAIL + write()), so
 * existing clients (wavplayer, pcm.c, ...) work unchanged.
 *
 * The gapless ring/slot/DMA-feeder plumbing is a direct port of the proven
 * soundd design; only the audio backend is swapped: instead of feeding the
 * analog PWM FIFO we feed the HDMI MAI_DATA FIFO with IEC60958 subframes and
 * program the VC4 HDMI audio block exactly like the Linux vc4_hdmi driver.
 */
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/ipc.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "hdmi_regs.h"

#define UNUSED(v) ((void)(v))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDMI_LOG(...) ((void)0)

#define CTRL_PCM_DEV_HW 0xF0
#define CTRL_PCM_DEV_HW_FREE 0xF1
#define CTRL_PCM_DEV_PRPARE 0xF2
#define CTRL_PCM_BUF_AVAIL 0xF3

/* DMA bus-address aliasing (same model the analog soundd uses). */
#define DMA_VC_ALIAS_UNCACHED 0xC0000000u
#define DMA_BUS_ADDR_MASK 0x3FFFFFFFu

/*
 * DMA channel 3: channel 4 is reserved for the DSI1 workaround and channel 5
 * is used by the analog soundd, so HDMI audio takes a free legacy channel.
 */
#define DMA_CHANNEL     3U
#define DMA_BASE        (_mmio_base + 0x007000 + (DMA_CHANNEL * 0x100u))
#define DMA_ENABLE      (_mmio_base + 0x007FF0)

#define DMA_CS        0
#define DMA_CONBLK_AD 1
#define DMA_ENABLE_BIT (1U << DMA_CHANNEL)
#define DMA_ACTIVE    1
#define DMA_RESET     (1U << 31)
#define DMA_PRIORITY_DEFAULT (8U << 16)
#define DMA_PANIC_PRIORITY_DEFAULT (8U << 20)
#define DMA_DEST_DREQ 0x40
#define DMA_SRC_INC   0x100

#define DMA_BUF_SIZE  (1024*4)
#define DMA_SAMPLE_CAPACITY (DMA_BUF_SIZE / sizeof(uint32_t))
#define DMA_BUFFER_SLOTS 32U
#define DMA_TOTAL_BUF_SIZE (DMA_BUF_SIZE * DMA_BUFFER_SLOTS)
#define DMA_SLOT_INVALID DMA_BUFFER_SLOTS
#define DMA_CHAIN_START_SLOTS 6U
#define DMA_CHAIN_REBUFFER_START_SLOTS 10U

#define HDMI_OUTPUT_CHANNELS 2U
#define SOUND_FEED_KICK_SLEEP_US 500U
#define SOUND_FEED_IDLE_SLEEP_US 1000U
#define SOUND_FEED_DEEP_IDLE_SLEEP_US 20000U
#define SOUND_FEED_GUARD_US 2000U
#define SOUND_DMA_START_TARGET_US 160000U
#define SOUND_DMA_REBUFFER_TARGET_US 224000U
#define SOUND_PCM_RING_MIN_BYTES (128U * 1024U)
#define SOUND_PCM_RING_MAX_BYTES (512U * 1024U)
#define SOUND_PCM_RING_BUFFER_MULTIPLIER 8U
#define DMA_START_IDLE_FLUSH_US 40000U
#define DMA_WATCHDOG_MARGIN_US 200000U
#define SOUND_DEFAULT_BIT_DEPTH 16
#define SOUND_DEFAULT_RATE 48000
#define SOUND_DEFAULT_CHANNELS 2
#define SOUND_DEFAULT_PERIOD_SIZE 1024
#define SOUND_DEFAULT_PERIOD_COUNT 4

/* HDMI video-mode refresh probe window used to derive the pixel clock. */
#define HDMI_REFRESH_PROBE_US 100000U
#define HDMI_DEFAULT_REFRESH_HZ 60U
#define HDMI_DEFAULT_PIXEL_CLOCK_HZ 74250000ULL  /* 720p/1080i-ish fallback */

#define DMA_SLOT_EMPTY   0U
#define DMA_SLOT_FILLING 1U
#define DMA_SLOT_READY   2U
#define DMA_SLOT_ACTIVE  3U

typedef struct dma_cb {
   unsigned int ti;
   unsigned int source_ad;
   unsigned int dest_ad;
   unsigned int txfr_len;
   unsigned int stride;
   unsigned int nextconbk;
   unsigned int null1;
   unsigned int null2;
} dma_cb_t;

struct pcm_config {
    int bit_depth;
    int rate;
    int channels;
    int period_size;
    int period_count;
    int start_threshold;
    int stop_threshold;
};

typedef struct {
    dma_cb_t* dma_cbs;
    ewokos_addr_t dma_cbs_addr;
    ewokos_addr_t dma_data_base_addr;
    ewokos_addr_t dma_cbs_phy;
    ewokos_addr_t dma_data_base_phy;
    struct pcm_config pcm_cfg;
    uint32_t frame_bytes;
    uint32_t period_bytes;
    uint32_t buffer_bytes;
    uint32_t write_chunk_bytes;
    uint8_t* pcm_ring;
    uint32_t pcm_ring_bytes;
    uint32_t pcm_ring_rd;
    uint32_t pcm_ring_wr;
    uint32_t pcm_ring_used;
    uint32_t* dma_slots[DMA_BUFFER_SLOTS];
    ewokos_addr_t dma_slot_phys[DMA_BUFFER_SLOTS];
    uint32_t slot_words[DMA_BUFFER_SLOTS];
    uint8_t slot_state[DMA_BUFFER_SLOTS];
    uint32_t ready_slots[DMA_BUFFER_SLOTS];
    uint32_t ready_head;
    uint32_t ready_count;
    uint32_t active_slots[DMA_BUFFER_SLOTS];
    uint32_t active_end_usec[DMA_BUFFER_SLOTS];
    uint32_t active_count;
    uint32_t active_tail_end_usec;
    uint32_t fill_slot;
    uint32_t dma_started_usec;
    uint32_t dma_expected_usec;
    uint32_t last_push_usec;
    uint32_t dma_watchdog_count;
    uint32_t rebuffer_restart_count;
    uint32_t feeder_last_loop_usec;
    uint32_t feeder_delay_count;
    uint32_t feeder_delay_max_usec;
    uint32_t ring_starve_count;
    uint64_t pixel_clock_hz;
    bool need_rebuffer;
    bool configured;
    bool prepared;
    bool started;
    bool dma_running;
    bool feeder_exit;
    int open_count;
    int occupied_pid;
} snd_dev_t;

static snd_dev_t _snd = {0};
static pthread_mutex_t _snd_lock;
static pthread_t _snd_feeder_tid;
static bool _snd_feeder_started = false;
static vdevice_t* _snd_dev = NULL;
static bool _snd_writer_parked = false;
static sys_info_t _sys_info;

/* Selected board geometry (points at one of the const tables in hdmi_regs.h). */
static const hdmi_board_t* _board = &HDMI_BOARD_BCM2835;
static uintptr_t _hd_base = 0;    /* VC4_HD audio block    */
static uintptr_t _core_base = 0;  /* VC4_HDMI core block   */
static uintptr_t _ram_base = 0;   /* infoframe packet RAM  */

static int audio_stop(void);
static uint32_t audio_elapsed_usec(uint32_t start_usec, uint32_t now_usec);
static uint32_t audio_active_remaining_usec(uint32_t now_usec);
static void* sound_feeder_thread(void* arg);

/* ---- register accessors --------------------------------------------- */

static inline uint32_t hd_rd(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void hd_wr(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(base + off) = val;
}

static inline uint32_t core_rd(uint32_t off) { return hd_rd(_core_base, off); }
static inline void core_wr(uint32_t off, uint32_t val) { hd_wr(_core_base, off, val); }
static inline uint32_t hdreg_rd(uint32_t off) { return hd_rd(_hd_base, off); }
static inline void hdreg_wr(uint32_t off, uint32_t val) { hd_wr(_hd_base, off, val); }

static void audio_detect_hw_config(void) {
    bool pi4 = _sys_info.mmio.phy_base == 0xfe000000u;

    _board = pi4 ? &HDMI_BOARD_BCM2711_HDMI0 : &HDMI_BOARD_BCM2835;
    _hd_base = _mmio_base + _board->hd_base_off;
    _core_base = _mmio_base + _board->core_base_off;
    _ram_base = _mmio_base + _board->ram_base_off;
    HDMI_LOG("hdmi_soundd: pi4=%d hd=%llx core=%llx ram=%llx mai_data=%x permap=%x\n",
            pi4 ? 1 : 0, (unsigned long long)_hd_base, (unsigned long long)_core_base,
            (unsigned long long)_ram_base, _board->mai_data_bus, _board->dma_permap);
}

/* ---- timing / pixel-clock derivation -------------------------------- */

static uint32_t audio_now_usec(void) {
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)(uint32_t)tv.tv_sec * 1000000ULL) +
            (uint64_t)(uint32_t)tv.tv_usec);
}

/* Extract a bit field given a mask whose low bit is the field's shift. */
static inline uint32_t field_get(uint32_t reg, uint32_t mask, uint32_t shift) {
    return (reg & mask) >> shift;
}

/*
 * Reconstruct htotal/vtotal from the HDMI timing registers. The layout of
 * these registers differs between vc4 (bcm2835) and vc5 (bcm2711).
 */
static void audio_read_video_timings(uint32_t* htotal, uint32_t* vtotal) {
    uint32_t horza = core_rd(_board->off_horza);
    uint32_t horzb = core_rd(_board->off_horzb);
    uint32_t verta = core_rd(_board->off_verta0);
    uint32_t vertb = core_rd(_board->off_vertb0);
    uint32_t hap, hfp, hsp, hbp, val, vfp, vsp, vbp;

    if (_board->pi4) {
        hap = field_get(horza, VC5_HDMI_HORZA_HAP_MASK, VC5_HDMI_HORZA_HAP_SHIFT);
        hfp = field_get(horza, VC5_HDMI_HORZA_HFP_MASK, VC5_HDMI_HORZA_HFP_SHIFT);
        hsp = field_get(horzb, VC5_HDMI_HORZB_HSP_MASK, VC5_HDMI_HORZB_HSP_SHIFT);
        hbp = field_get(horzb, VC5_HDMI_HORZB_HBP_MASK, VC5_HDMI_HORZB_HBP_SHIFT);
        val = field_get(verta, VC5_HDMI_VERTA_VAL_MASK, VC5_HDMI_VERTA_VAL_SHIFT);
        vfp = field_get(verta, VC5_HDMI_VERTA_VFP_MASK, VC5_HDMI_VERTA_VFP_SHIFT);
        vsp = field_get(verta, VC5_HDMI_VERTA_VSP_MASK, VC5_HDMI_VERTA_VSP_SHIFT);
    }
    else {
        hap = field_get(horza, VC4_HDMI_HORZA_HAP_MASK, 0);
        hfp = field_get(horzb, VC4_HDMI_HORZB_HFP_MASK, VC4_HDMI_HORZB_HFP_SHIFT);
        hsp = field_get(horzb, VC4_HDMI_HORZB_HSP_MASK, VC4_HDMI_HORZB_HSP_SHIFT);
        hbp = field_get(horzb, VC4_HDMI_HORZB_HBP_MASK, VC4_HDMI_HORZB_HBP_SHIFT);
        val = field_get(verta, VC4_HDMI_VERTA_VAL_MASK, VC4_HDMI_VERTA_VAL_SHIFT);
        vfp = field_get(verta, VC4_HDMI_VERTA_VFP_MASK, VC4_HDMI_VERTA_VFP_SHIFT);
        vsp = field_get(verta, VC4_HDMI_VERTA_VSP_MASK, VC4_HDMI_VERTA_VSP_SHIFT);
    }
    vbp = field_get(vertb, VC4_HDMI_VERTB_VBP_MASK, VC4_HDMI_VERTB_VBP_SHIFT);

    *htotal = hap + hfp + hsp + hbp;
    *vtotal = val + vfp + vsp + vbp;
}

/*
 * Measure the active refresh rate using the free-running HDMI frame counter,
 * then derive pixel_clock = htotal * vtotal * refresh. This mirrors the
 * mode->clock the Linux driver uses for the N/CTS and HSM computations, but
 * without needing a DRM modeline (we read it back from the hardware the
 * firmware already programmed).
 */
static uint64_t audio_measure_pixel_clock(void) {
    /* The video mode is fixed for the session, so a successful frame-counter
     * measurement is cached: it avoids a ~100ms usleep inside the IPC handler
     * on every CTRL_PCM_DEV_HW. A mode-not-up result is deliberately NOT
     * cached, so a display that comes up later is measured correctly. */
    static uint64_t cached = 0;
    uint32_t htotal = 0, vtotal = 0;
    uint32_t f0, f1, t0, t1, dt, df;
    uint64_t pixel;

    if (cached != 0) {
        return cached;
    }

    audio_read_video_timings(&htotal, &vtotal);
    if (htotal < 100 || htotal > 8192 || vtotal < 100 || vtotal > 8192) {
        return HDMI_DEFAULT_PIXEL_CLOCK_HZ;
    }

    f0 = hdreg_rd(_board->off_frame_count);
    t0 = audio_now_usec();
    usleep(HDMI_REFRESH_PROBE_US);
    f1 = hdreg_rd(_board->off_frame_count);
    t1 = audio_now_usec();

    dt = t1 - t0;
    df = f1 - f0;
    if (dt == 0 || df == 0) {
        /* Frame counter not advancing: assume a standard 60Hz mode. */
        pixel = (uint64_t)htotal * (uint64_t)vtotal * HDMI_DEFAULT_REFRESH_HZ;
        return pixel;
    }

    /* refresh_hz = df * 1e6 / dt ; pixel = htotal * vtotal * refresh_hz */
    pixel = (uint64_t)htotal * (uint64_t)vtotal * (uint64_t)df * 1000000ULL;
    pixel /= (uint64_t)dt;
    if (pixel < 1000000ULL || pixel > 600000000ULL) {
        pixel = (uint64_t)htotal * (uint64_t)vtotal * HDMI_DEFAULT_REFRESH_HZ;
    }
    cached = pixel;
    return pixel;
}

/* ---- MAI clock helpers ---------------------------------------------- */

/*
 * Continued-fraction best rational approximation (lib/math/rational.c), used
 * to derive the MAI_SMP N/M divider so that samplerate ~= hsm * (M+1) / N.
 */
static void rational_best_approximation(unsigned long numerator,
        unsigned long denominator, unsigned long max_numerator,
        unsigned long max_denominator, unsigned long* bp_numerator,
        unsigned long* bp_denominator) {
    unsigned long n, d, n0, d0, n1, d1;

    n = numerator;
    d = denominator;
    n0 = d1 = 0;
    d0 = n1 = 1;
    for (;;) {
        unsigned long t, a;

        /* Revert as a whole convergent pair if either bound is exceeded, so
         * the returned n/d is always a genuine (consistent) approximation. */
        if ((n1 > max_numerator) || (d1 > max_denominator)) {
            n1 = n0;
            d1 = d0;
            break;
        }
        if (d == 0) {
            break;
        }
        t = d;
        a = n / d;
        d = n % d;
        n = t;
        t = n0 + a * n1;
        n0 = n1;
        n1 = t;
        t = d0 + a * d1;
        d0 = d1;
        d1 = t;
    }
    *bp_numerator = n1;
    *bp_denominator = d1;
}

static void audio_set_mai_clock(unsigned int samplerate, uint64_t pixel_clock) {
    uint64_t hsm;
    unsigned long n = 1, m = 1;

    /* HSM clock floor used by vc4: hsm = max(120MHz, tmds_char_rate * 1.01). */
    hsm = pixel_clock + (pixel_clock / 100ULL);
    if (hsm < HDMI_HSM_MIN_CLOCK_FREQ) {
        hsm = HDMI_HSM_MIN_CLOCK_FREQ;
    }

    rational_best_approximation((unsigned long)hsm, samplerate,
            VC4_HD_MAI_SMP_N_MASK >> VC4_HD_MAI_SMP_N_SHIFT,
            (VC4_HD_MAI_SMP_M_MASK >> VC4_HD_MAI_SMP_M_SHIFT) + 1,
            &n, &m);
    if (m == 0) {
        m = 1;
    }
    hdreg_wr(_board->off_mai_smp,
            ((n << VC4_HD_MAI_SMP_N_SHIFT) & VC4_HD_MAI_SMP_N_MASK) |
            ((m - 1) & VC4_HD_MAI_SMP_M_MASK));
}

static void audio_set_n_cts(unsigned int samplerate, uint64_t pixel_clock) {
    uint32_t n;
    uint32_t cts;

    /*
     * N is NOT 128*rate/1000. The 44.1 kHz family must use the HDMI-spec N
     * values (6272 / 12544 / 25088) so that CTS = pixel_clock*N/(128*fs) stays
     * integral at the standard pixel clocks -- this is exactly the switch() in
     * Linux vc4_hdmi_set_n_cts(). The old formula computed 5644 for 44100
     * (instead of 6272) and paired it with cts = pixel_clock/1000, so a 44.1 kHz
     * stream drove the sink with a wrong regenerated audio clock and stayed
     * silent, while 48 kHz (whose 128*48 == 6144 already matches the spec)
     * played fine. CTS must be derived from the chosen N, not assumed.
     */
    switch (samplerate) {
    case 32000:  n = 4096u;  break;
    case 44100:  n = 6272u;  break;
    case 48000:  n = 6144u;  break;
    case 88200:  n = 12544u; break;
    case 96000:  n = 12288u; break;
    case 176400: n = 25088u; break;
    case 192000: n = 24576u; break;
    default:     n = 128u * samplerate / 1000u; break;
    }

    cts = (uint32_t)((pixel_clock * n) / (128ULL * samplerate));

    core_wr(_board->off_crp_cfg,
            VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN |
            ((n << VC4_HDMI_CRP_CFG_N_SHIFT) & VC4_HDMI_CRP_CFG_N_MASK));
    core_wr(_board->off_cts_0, cts);
    core_wr(_board->off_cts_1, cts);
}

static uint32_t sample_rate_to_mai_fmt(int samplerate) {
    switch (samplerate) {
    case 8000:   return VC4_HDMI_MAI_SAMPLE_RATE_8000;
    case 11025:  return VC4_HDMI_MAI_SAMPLE_RATE_11025;
    case 12000:  return VC4_HDMI_MAI_SAMPLE_RATE_12000;
    case 16000:  return VC4_HDMI_MAI_SAMPLE_RATE_16000;
    case 22050:  return VC4_HDMI_MAI_SAMPLE_RATE_22050;
    case 24000:  return VC4_HDMI_MAI_SAMPLE_RATE_24000;
    case 32000:  return VC4_HDMI_MAI_SAMPLE_RATE_32000;
    case 44100:  return VC4_HDMI_MAI_SAMPLE_RATE_44100;
    case 48000:  return VC4_HDMI_MAI_SAMPLE_RATE_48000;
    case 64000:  return VC4_HDMI_MAI_SAMPLE_RATE_64000;
    case 88200:  return VC4_HDMI_MAI_SAMPLE_RATE_88200;
    case 96000:  return VC4_HDMI_MAI_SAMPLE_RATE_96000;
    case 128000: return VC4_HDMI_MAI_SAMPLE_RATE_128000;
    case 176400: return VC4_HDMI_MAI_SAMPLE_RATE_176400;
    case 192000: return VC4_HDMI_MAI_SAMPLE_RATE_192000;
    default:     return VC4_HDMI_MAI_SAMPLE_RATE_NOT_INDICATED;
    }
}

/* CEA-861 audio infoframe sampling-frequency code. */
static uint32_t sample_rate_to_infoframe_freq(int samplerate) {
    switch (samplerate) {
    case 32000:  return 1;
    case 44100:  return 2;
    case 48000:  return 3;
    case 88200:  return 4;
    case 96000:  return 5;
    case 176400: return 6;
    case 192000: return 7;
    default:     return 0;  /* refer to stream header */
    }
}

/* CEA-861 audio infoframe sample-size code. */
static uint32_t bit_depth_to_infoframe_size(int bit_depth) {
    switch (bit_depth) {
    case 16: return 1;
    case 20: return 2;
    case 24: return 3;
    default: return 0;  /* refer to stream header */
    }
}


/* ---- PCM -> IEC60958 subframe conversion ---------------------------- */

static uint32_t audio_output_words_per_frame(void) {
    return HDMI_OUTPUT_CHANNELS;
}

/*
 * The VC4 MAI consumes IEC60958 subframe words (SNDRV_PCM_FMTBIT_
 * IEC958_SUBFRAME_LE upstream). Each 32-bit word carries one channel:
 *   bits[27:4] = 24-bit audio sample, left justified
 *   bit 28 = V(valid, 0), bit 29 = U(0), bit 30 = C(0), bit 31 = P(parity)
 * The preamble/BMC encoding is done downstream by the HDMI packetizer.
 */
static uint32_t iec958_subframe(int32_t s32) {
    uint32_t w = ((uint32_t)s32 >> 4) & 0x0FFFFFF0u;  /* sample into bits[27:4] */
    uint32_t v = (w >> 4) & 0x07FFFFFFu;              /* bits[30:4] */

    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    if (v & 1u) {
        w |= (1u << 31);  /* even parity over bits[4..31] */
    }
    return w;
}

static int32_t audio_pcm_sample_to_s32(const uint8_t* data, uint32_t sample_bytes) {
    switch (sample_bytes) {
    case 1:
        return ((int32_t)data[0] - 128) * 16777216;
    case 2: {
        int16_t v = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
        return (int32_t)v * 65536;
    }
    case 3: {
        int32_t v = (int32_t)((uint32_t)data[0] |
                ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16));
        if ((v & 0x00800000) != 0) {
            v |= ~0x00FFFFFF;
        }
        return v * 256;
    }
    case 4:
        return (int32_t)((uint32_t)data[0] |
                ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16) |
                ((uint32_t)data[3] << 24));
    default:
        return 0;
    }
}

static uint32_t audio_sample_bytes(int bit_depth) {
    switch (bit_depth) {
    case 8:  return 1;
    case 16: return 2;
    case 24: return 3;
    case 32: return 4;
    default: return 0;
    }
}

static uint32_t audio_silence_word(void) {
    return 0;  /* IEC958 subframe for a zero sample is all-zero */
}

static void audio_convert_s16_stereo_frames(const uint8_t* src, uint32_t* dst, uint32_t frames) {
    const uint8_t* p = src;
    uint32_t* q = dst;
    uint32_t i;

    for (i = 0; i < frames; i++) {
        int16_t left = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        int16_t right = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));

        q[0] = iec958_subframe((int32_t)left << 16);
        q[1] = iec958_subframe((int32_t)right << 16);
        p += 4;
        q += 2;
    }
}

static void audio_frame_to_mai_words(const uint8_t* frame, uint32_t* words) {
    uint32_t sample_bytes;
    int32_t left;
    int32_t right;

    if (_snd.pcm_cfg.bit_depth == 16) {
        int16_t left16 = (int16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
        int16_t right16 = left16;

        if (_snd.pcm_cfg.channels > 1) {
            right16 = (int16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
        }
        words[0] = iec958_subframe((int32_t)left16 << 16);
        words[1] = iec958_subframe((int32_t)right16 << 16);
        return;
    }

    sample_bytes = audio_sample_bytes(_snd.pcm_cfg.bit_depth);
    left = audio_pcm_sample_to_s32(frame, sample_bytes);
    if (_snd.pcm_cfg.channels > 1) {
        right = audio_pcm_sample_to_s32(frame + sample_bytes, sample_bytes);
    }
    else {
        right = left;
    }
    words[0] = iec958_subframe(left);
    words[1] = iec958_subframe(right);
}

/* ---- ring buffer ---------------------------------------------------- */

static void audio_queue_reset(void) {
    memset(_snd.slot_words, 0, sizeof(_snd.slot_words));
    memset(_snd.slot_state, 0, sizeof(_snd.slot_state));
    memset(_snd.ready_slots, 0, sizeof(_snd.ready_slots));
    memset(_snd.active_slots, 0, sizeof(_snd.active_slots));
    memset(_snd.active_end_usec, 0, sizeof(_snd.active_end_usec));
    _snd.ready_head = 0;
    _snd.ready_count = 0;
    _snd.active_count = 0;
    _snd.active_tail_end_usec = 0;
    _snd.fill_slot = 0;
    _snd.last_push_usec = 0;
    _snd.need_rebuffer = true;
    if (DMA_BUFFER_SLOTS > 0) {
        _snd.slot_state[0] = DMA_SLOT_FILLING;
    }
}

static void audio_pcm_ring_reset(void) {
    _snd.pcm_ring_rd = 0;
    _snd.pcm_ring_wr = 0;
    _snd.pcm_ring_used = 0;
}

static uint32_t audio_pcm_ring_pending_bytes(void) {
    return _snd.pcm_ring_used;
}

static uint32_t audio_pcm_ring_avail_bytes(void) {
    if (_snd.pcm_ring_bytes <= _snd.pcm_ring_used) {
        return 0;
    }
    return _snd.pcm_ring_bytes - _snd.pcm_ring_used;
}

static uint32_t audio_pcm_ring_contig_read_bytes(void) {
    if (_snd.pcm_ring == NULL || _snd.pcm_ring_used == 0) {
        return 0;
    }
    if (_snd.pcm_ring_rd < _snd.pcm_ring_wr) {
        return _snd.pcm_ring_wr - _snd.pcm_ring_rd;
    }
    return _snd.pcm_ring_bytes - _snd.pcm_ring_rd;
}

static uint32_t audio_pcm_ring_write_bytes(const uint8_t* src, uint32_t size) {
    uint32_t first;
    uint32_t second;

    if (_snd.pcm_ring == NULL || size == 0) {
        return 0;
    }
    if (size > audio_pcm_ring_avail_bytes()) {
        size = audio_pcm_ring_avail_bytes();
    }
    first = MIN(size, _snd.pcm_ring_bytes - _snd.pcm_ring_wr);
    memcpy(_snd.pcm_ring + _snd.pcm_ring_wr, src, first);
    second = size - first;
    if (second != 0) {
        memcpy(_snd.pcm_ring, src + first, second);
    }
    _snd.pcm_ring_wr = (_snd.pcm_ring_wr + size) % _snd.pcm_ring_bytes;
    _snd.pcm_ring_used += size;
    return size;
}

static void audio_pcm_ring_consume_bytes(uint32_t size) {
    if (size == 0 || _snd.pcm_ring == NULL) {
        return;
    }
    if (size > _snd.pcm_ring_used) {
        size = _snd.pcm_ring_used;
    }
    _snd.pcm_ring_rd = (_snd.pcm_ring_rd + size) % _snd.pcm_ring_bytes;
    _snd.pcm_ring_used -= size;
}

static uint32_t audio_pcm_ring_capacity_bytes(uint32_t frame_bytes) {
    uint32_t ring_bytes;

    ring_bytes = _snd.buffer_bytes * SOUND_PCM_RING_BUFFER_MULTIPLIER;
    if (ring_bytes < SOUND_PCM_RING_MIN_BYTES) {
        ring_bytes = SOUND_PCM_RING_MIN_BYTES;
    }
    if (ring_bytes > SOUND_PCM_RING_MAX_BYTES) {
        ring_bytes = SOUND_PCM_RING_MAX_BYTES;
    }
    if (frame_bytes != 0) {
        ring_bytes = (ring_bytes / frame_bytes) * frame_bytes;
    }
    if (ring_bytes < frame_bytes) {
        ring_bytes = frame_bytes;
    }
    return ring_bytes;
}

/* ---- queue accounting ----------------------------------------------- */

static uint32_t audio_queue_pending_words(void) {
    uint32_t words = 0;
    uint32_t i;

    for (i = 0; i < DMA_BUFFER_SLOTS; i++) {
        if (_snd.slot_state[i] != DMA_SLOT_EMPTY) {
            words += _snd.slot_words[i];
        }
    }
    return words;
}

static uint32_t audio_queue_avail_words(void) {
    uint32_t total_words = DMA_SAMPLE_CAPACITY * DMA_BUFFER_SLOTS;
    uint32_t used_words = audio_queue_pending_words();

    if (used_words >= total_words) {
        return 0;
    }
    return total_words - used_words;
}

static uint32_t audio_queue_avail_frames(void) {
    return audio_queue_avail_words() / audio_output_words_per_frame();
}

static uint32_t audio_queue_avail_bytes(void) {
    return audio_queue_avail_frames() * _snd.frame_bytes;
}

static uint32_t audio_words_to_usec(uint32_t words) {
    if (_snd.pcm_cfg.rate <= 0 || words == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)words * 1000000ULL) /
            ((uint64_t)_snd.pcm_cfg.rate * (uint64_t)audio_output_words_per_frame()));
}

static uint32_t audio_slot_duration_usec(void) {
    return audio_words_to_usec(DMA_SAMPLE_CAPACITY);
}

static bool sound_has_pending_work(void) {
    return _snd.dma_running ||
            audio_queue_pending_words() > 0 ||
            audio_pcm_ring_pending_bytes() > 0;
}

static uint32_t audio_elapsed_usec(uint32_t start_usec, uint32_t now_usec) {
    return now_usec - start_usec;
}

static uint32_t audio_dma_samples_usec(uint32_t samples) {
    uint64_t words_per_frame = audio_output_words_per_frame();

    if (_snd.pcm_cfg.rate <= 0 || samples == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)samples * 1000000ULL) /
            ((uint64_t)_snd.pcm_cfg.rate * words_per_frame));
}

static uint32_t audio_dma_watchdog_usec(uint32_t samples) {
    uint64_t expected;
    uint64_t words_per_frame = audio_output_words_per_frame();

    if (_snd.pcm_cfg.rate <= 0 || samples == 0) {
        return DMA_WATCHDOG_MARGIN_US;
    }
    expected = ((uint64_t)samples * 1000000ULL) /
            ((uint64_t)_snd.pcm_cfg.rate * words_per_frame);
    expected += DMA_WATCHDOG_MARGIN_US;
    if (expected > 1000000ULL) {
        expected = 1000000ULL;
    }
    return (uint32_t)expected;
}

static uint32_t audio_active_remaining_usec(uint32_t now_usec) {
    uint32_t elapsed_usec;

    if (!_snd.dma_running || _snd.dma_started_usec == 0 || _snd.active_count == 0) {
        return 0;
    }
    elapsed_usec = audio_elapsed_usec(_snd.dma_started_usec, now_usec);
    if (elapsed_usec >= _snd.active_tail_end_usec) {
        return 0;
    }
    return _snd.active_tail_end_usec - elapsed_usec;
}

static uint32_t sound_feeder_sleep_usec(uint32_t now_usec) {
    uint32_t remaining_usec;

    if (_snd.open_count <= 0 &&
            !_snd.configured && !_snd.prepared && !_snd.started &&
            !sound_has_pending_work()) {
        return SOUND_FEED_DEEP_IDLE_SLEEP_US;
    }
    if (audio_pcm_ring_pending_bytes() > 0) {
        if (!_snd.started || !_snd.dma_running) {
            return SOUND_FEED_KICK_SLEEP_US;
        }
    }
    if (_snd.dma_running) {
        remaining_usec = audio_active_remaining_usec(now_usec);
        if (remaining_usec > (SOUND_FEED_GUARD_US * 2U)) {
            uint32_t wait_usec = remaining_usec - SOUND_FEED_GUARD_US;
            if (wait_usec > SOUND_FEED_IDLE_SLEEP_US) {
                wait_usec = SOUND_FEED_IDLE_SLEEP_US;
            }
            return wait_usec;
        }
        return SOUND_FEED_KICK_SLEEP_US;
    }
    if (sound_has_pending_work()) {
        return SOUND_FEED_KICK_SLEEP_US;
    }
    return SOUND_FEED_IDLE_SLEEP_US;
}

static uint32_t audio_queue_start_words_threshold(void) {
    uint32_t threshold;
    uint32_t min_batch_words = (DMA_SAMPLE_CAPACITY * 3U) / 4U;

    if (_snd.frame_bytes == 0) {
        return DMA_SAMPLE_CAPACITY;
    }
    threshold = (_snd.period_bytes / _snd.frame_bytes) * audio_output_words_per_frame();
    if (threshold < min_batch_words) {
        threshold = min_batch_words;
    }
    if (threshold == 0 || threshold > DMA_SAMPLE_CAPACITY) {
        threshold = DMA_SAMPLE_CAPACITY;
    }
    return threshold;
}

static uint32_t audio_queue_rebuffer_words_threshold(void) {
    uint32_t threshold = audio_queue_start_words_threshold() * 2U;
    uint32_t min_rebuffer_slots = DMA_CHAIN_REBUFFER_START_SLOTS;
    uint32_t min_rebuffer_words;
    uint32_t max_words = DMA_SAMPLE_CAPACITY * DMA_BUFFER_SLOTS;

    if (min_rebuffer_slots == 0 || min_rebuffer_slots > DMA_BUFFER_SLOTS) {
        min_rebuffer_slots = DMA_BUFFER_SLOTS;
    }
    min_rebuffer_words = DMA_SAMPLE_CAPACITY * min_rebuffer_slots;
    if (threshold < min_rebuffer_words) {
        threshold = min_rebuffer_words;
    }
    if (threshold > max_words) {
        threshold = max_words;
    }
    return threshold;
}

static uint32_t audio_queue_streaming_words_threshold(void) {
    uint32_t threshold;
    uint32_t min_threshold = DMA_SAMPLE_CAPACITY / 16U;
    uint32_t period_words;

    if (_snd.frame_bytes == 0) {
        return min_threshold;
    }
    period_words = (_snd.period_bytes / _snd.frame_bytes) * audio_output_words_per_frame();
    threshold = period_words / 2U;
    if (threshold == 0) {
        threshold = 1;
    }
    if (threshold < min_threshold) {
        threshold = min_threshold;
    }
    if (threshold > DMA_SAMPLE_CAPACITY) {
        threshold = DMA_SAMPLE_CAPACITY;
    }
    return threshold;
}

static uint32_t audio_queue_start_slot_limit(void) {
    uint32_t min_slots;
    uint32_t target_usec;
    uint32_t slot_usec;
    uint32_t target_slots;

    if (_snd.need_rebuffer) {
        min_slots = DMA_CHAIN_REBUFFER_START_SLOTS;
        target_usec = SOUND_DMA_REBUFFER_TARGET_US;
    }
    else {
        min_slots = DMA_CHAIN_START_SLOTS;
        target_usec = SOUND_DMA_START_TARGET_US;
    }
    if (min_slots == 0) {
        min_slots = 1U;
    }
    if (min_slots > DMA_BUFFER_SLOTS) {
        min_slots = DMA_BUFFER_SLOTS;
    }
    slot_usec = audio_slot_duration_usec();
    target_slots = min_slots;
    if (slot_usec != 0 && target_usec > 0) {
        uint32_t computed_slots = (target_usec + slot_usec - 1U) / slot_usec;
        if (computed_slots > target_slots) {
            target_slots = computed_slots;
        }
    }
    if (target_slots > DMA_BUFFER_SLOTS) {
        target_slots = DMA_BUFFER_SLOTS;
    }
    return target_slots;
}

/* ---- slot filling ---------------------------------------------------- */

static uint32_t audio_find_empty_slot(void) {
    uint32_t i;

    for (i = 0; i < DMA_BUFFER_SLOTS; i++) {
        if (_snd.slot_state[i] == DMA_SLOT_EMPTY) {
            return i;
        }
    }
    return DMA_SLOT_INVALID;
}

static bool audio_ensure_fill_slot(void) {
    uint32_t slot;

    if (_snd.fill_slot < DMA_BUFFER_SLOTS &&
            _snd.slot_state[_snd.fill_slot] == DMA_SLOT_FILLING) {
        return true;
    }
    slot = audio_find_empty_slot();
    if (slot == DMA_SLOT_INVALID) {
        return false;
    }
    _snd.fill_slot = slot;
    _snd.slot_state[slot] = DMA_SLOT_FILLING;
    _snd.slot_words[slot] = 0;
    return true;
}

static bool audio_queue_finalize_fill_slot(void) {
    uint32_t tail;
    uint32_t slot = _snd.fill_slot;

    if (slot >= DMA_BUFFER_SLOTS ||
            _snd.slot_state[slot] != DMA_SLOT_FILLING ||
            _snd.slot_words[slot] == 0 ||
            _snd.ready_count >= DMA_BUFFER_SLOTS) {
        return false;
    }
    _snd.slot_state[slot] = DMA_SLOT_READY;
    tail = (_snd.ready_head + _snd.ready_count) % DMA_BUFFER_SLOTS;
    _snd.ready_slots[tail] = slot;
    _snd.ready_count++;
    _snd.fill_slot = DMA_SLOT_INVALID;
    return true;
}

static bool audio_queue_force_finalize_fill_slot(void) {
    uint32_t slot = _snd.fill_slot;
    uint32_t silence;
    uint32_t* dst;
    uint32_t remain;
    uint32_t i;

    if (slot >= DMA_BUFFER_SLOTS ||
            _snd.slot_state[slot] != DMA_SLOT_FILLING ||
            _snd.slot_words[slot] == 0) {
        return false;
    }
    if (_snd.slot_words[slot] < DMA_SAMPLE_CAPACITY) {
        silence = audio_silence_word();
        dst = _snd.dma_slots[slot] + _snd.slot_words[slot];
        remain = DMA_SAMPLE_CAPACITY - _snd.slot_words[slot];
        for (i = 0; i < remain; ++i) {
            dst[i] = silence;
        }
        _snd.slot_words[slot] = DMA_SAMPLE_CAPACITY;
    }
    return audio_queue_finalize_fill_slot();
}

static uint32_t audio_queue_push_pcm(const uint8_t* buf, int size) {
    uint32_t frames = (uint32_t)(size / (int)_snd.frame_bytes);
    uint32_t avail = audio_queue_avail_frames();
    uint32_t pushed = 0;

    if (_snd.dma_data_base_addr == 0 || frames > avail) {
        return 0;
    }
    while (pushed < frames) {
        uint32_t slot;
        uint32_t frame_cap;
        uint32_t chunk_frames;
        uint32_t* dst;
        uint32_t i;

        if (!audio_ensure_fill_slot()) {
            break;
        }
        slot = _snd.fill_slot;
        frame_cap = (DMA_SAMPLE_CAPACITY - _snd.slot_words[slot]) /
                audio_output_words_per_frame();
        if (frame_cap == 0) {
            audio_queue_finalize_fill_slot();
            continue;
        }
        chunk_frames = frames - pushed;
        if (chunk_frames > frame_cap) {
            chunk_frames = frame_cap;
        }
        dst = _snd.dma_slots[slot] + _snd.slot_words[slot];
        if (_snd.pcm_cfg.bit_depth == 16 && _snd.pcm_cfg.channels == 2) {
            audio_convert_s16_stereo_frames(buf + (pushed * _snd.frame_bytes),
                    dst, chunk_frames);
        }
        else {
            for (i = 0; i < chunk_frames; i++) {
                audio_frame_to_mai_words(buf + ((pushed + i) * _snd.frame_bytes),
                        dst + (i * audio_output_words_per_frame()));
            }
        }
        _snd.slot_words[slot] += chunk_frames * audio_output_words_per_frame();
        pushed += chunk_frames;
        if (_snd.slot_words[slot] == DMA_SAMPLE_CAPACITY) {
            audio_queue_finalize_fill_slot();
        }
    }
    if (pushed != 0) {
        _snd.last_push_usec = audio_now_usec();
    }
    return pushed;
}

/* ---- DMA chain ------------------------------------------------------- */

static dma_cb_t* audio_slot_dma_cb(uint32_t slot) {
    if (_snd.dma_cbs == NULL || slot >= DMA_BUFFER_SLOTS) {
        return NULL;
    }
    return &_snd.dma_cbs[slot];
}

static ewokos_addr_t audio_slot_dma_cb_bus(uint32_t slot) {
    return (_snd.dma_cbs_phy +
            (ewokos_addr_t)(slot * sizeof(dma_cb_t))) | DMA_VC_ALIAS_UNCACHED;
}

static uint32_t audio_dma_cb_bus_normalize(uint32_t cb_bus) {
    return cb_bus & DMA_BUS_ADDR_MASK;
}

static void audio_queue_complete_active_chain(void) {
    uint32_t i;

    for (i = 0; i < _snd.active_count; i++) {
        uint32_t slot = _snd.active_slots[i];
        if (slot >= DMA_BUFFER_SLOTS) {
            continue;
        }
        _snd.slot_words[slot] = 0;
        _snd.slot_state[slot] = DMA_SLOT_EMPTY;
    }
    _snd.active_count = 0;
}

static void audio_queue_maybe_finalize_fill_slot(uint32_t now_usec) {
    uint32_t words;
    uint32_t threshold = audio_queue_start_words_threshold();
    uint32_t stream_threshold = audio_queue_streaming_words_threshold();
    uint32_t early_stream_threshold = stream_threshold / 2U;
    uint32_t stream_window_usec = audio_dma_samples_usec(stream_threshold * 2U);
    uint32_t active_remaining_usec = audio_active_remaining_usec(now_usec);
    bool idle_flush;
    bool stream_flush;

    if (early_stream_threshold < (DMA_SAMPLE_CAPACITY / 32U)) {
        early_stream_threshold = DMA_SAMPLE_CAPACITY / 32U;
    }
    if (early_stream_threshold == 0) {
        early_stream_threshold = 1;
    }
    if (_snd.fill_slot >= DMA_BUFFER_SLOTS ||
            _snd.slot_state[_snd.fill_slot] != DMA_SLOT_FILLING) {
        return;
    }
    words = _snd.slot_words[_snd.fill_slot];
    if (words == 0) {
        return;
    }
    idle_flush = (_snd.last_push_usec != 0) &&
            (audio_elapsed_usec(_snd.last_push_usec, now_usec) >= DMA_START_IDLE_FLUSH_US);
    stream_flush = _snd.dma_running &&
            _snd.active_count < DMA_BUFFER_SLOTS &&
            words >= ((_snd.ready_count <= 1 || _snd.active_count <= 2 ||
                    active_remaining_usec <= stream_window_usec) ?
                    early_stream_threshold : stream_threshold) &&
            (_snd.ready_count <= 1 ||
             _snd.active_count <= 2 ||
             active_remaining_usec <= stream_window_usec);
    if (words < threshold && !idle_flush && !stream_flush) {
        return;
    }
    audio_queue_finalize_fill_slot();
}

static bool audio_dma_active(void) {
    volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
    return ((*(dma + DMA_CS)) & DMA_ACTIVE) != 0;
}

static uint32_t audio_dma_current_active_slot(void) {
    volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
    uint32_t cb_bus = *(dma + DMA_CONBLK_AD);
    uint32_t cb_bus_norm = audio_dma_cb_bus_normalize(cb_bus);
    uint32_t i;

    for (i = 0; i < _snd.active_count; i++) {
        uint32_t slot = _snd.active_slots[i];
        if (slot >= DMA_BUFFER_SLOTS) {
            continue;
        }
        if (audio_dma_cb_bus_normalize((uint32_t)audio_slot_dma_cb_bus(slot)) == cb_bus_norm) {
            return i;
        }
    }
    return DMA_SLOT_INVALID;
}

static bool audio_queue_release_scheduled_active(uint32_t now_usec) {
    bool released = false;
    uint32_t current_active_idx;

    UNUSED(now_usec);
    if (!_snd.dma_running || _snd.dma_started_usec == 0 || _snd.active_count == 0) {
        return false;
    }
    current_active_idx = audio_dma_current_active_slot();
    if (current_active_idx == DMA_SLOT_INVALID) {
        return false;
    }
    while (_snd.active_count > 0 && current_active_idx > 0) {
        uint32_t slot = _snd.active_slots[0];
        uint32_t i;

        _snd.slot_words[slot] = 0;
        _snd.slot_state[slot] = DMA_SLOT_EMPTY;
        for (i = 1; i < _snd.active_count; i++) {
            _snd.active_slots[i - 1] = _snd.active_slots[i];
            _snd.active_end_usec[i - 1] = _snd.active_end_usec[i];
        }
        _snd.active_count--;
        current_active_idx--;
        released = true;
    }
    if (_snd.active_count == 0) {
        _snd.active_tail_end_usec = 0;
    }
    return released;
}

static uint32_t audio_queue_append_ready_chain(uint32_t now_usec) {
    uint32_t appended_samples = 0;
    uint32_t tail_slot;
    dma_cb_t* tail_cb;

    audio_queue_maybe_finalize_fill_slot(now_usec);
    if (!_snd.dma_running || _snd.ready_count == 0 || _snd.active_count == 0) {
        return 0;
    }
    tail_slot = _snd.active_slots[_snd.active_count - 1];
    tail_cb = audio_slot_dma_cb(tail_slot);
    if (tail_cb == NULL) {
        return 0;
    }
    while (_snd.ready_count > 0 && _snd.active_count < DMA_BUFFER_SLOTS) {
        uint32_t slot = _snd.ready_slots[_snd.ready_head];
        dma_cb_t* cb;

        _snd.ready_head = (_snd.ready_head + 1) % DMA_BUFFER_SLOTS;
        _snd.ready_count--;
        cb = audio_slot_dma_cb(slot);
        if (cb == NULL) {
            continue;
        }
        cb->source_ad = (uint32_t)_snd.dma_slot_phys[slot] | DMA_VC_ALIAS_UNCACHED;
        cb->dest_ad = _board->mai_data_bus;
        cb->txfr_len = _snd.slot_words[slot] * sizeof(uint32_t);
        cb->stride = 0x00;
        cb->nextconbk = 0x00;
        cb->null1 = 0x00;
        cb->null2 = 0x00;
        tail_cb->nextconbk = (uint32_t)audio_slot_dma_cb_bus(slot);
        _snd.slot_state[slot] = DMA_SLOT_ACTIVE;
        _snd.active_tail_end_usec += audio_dma_samples_usec(_snd.slot_words[slot]);
        _snd.active_slots[_snd.active_count] = slot;
        _snd.active_end_usec[_snd.active_count] = _snd.active_tail_end_usec;
        _snd.active_count++;
        appended_samples += _snd.slot_words[slot];
        tail_slot = slot;
        tail_cb = cb;
    }
    return appended_samples;
}

static bool audio_queue_prepare_dma_chain(uint32_t now_usec, uint32_t* head_slot,
        uint32_t* samples) {
    uint32_t tail_slot = DMA_SLOT_INVALID;

    audio_queue_maybe_finalize_fill_slot(now_usec);
    if (_snd.ready_count == 0) {
        return false;
    }
    _snd.active_count = 0;
    _snd.active_tail_end_usec = 0;
    *samples = 0;
    *head_slot = DMA_SLOT_INVALID;
    while (_snd.ready_count > 0 &&
            _snd.active_count < audio_queue_start_slot_limit()) {
        uint32_t slot = _snd.ready_slots[_snd.ready_head];
        dma_cb_t* cb;

        _snd.ready_head = (_snd.ready_head + 1) % DMA_BUFFER_SLOTS;
        _snd.ready_count--;
        cb = audio_slot_dma_cb(slot);
        if (cb == NULL) {
            continue;
        }
        cb->source_ad = (uint32_t)_snd.dma_slot_phys[slot] | DMA_VC_ALIAS_UNCACHED;
        cb->dest_ad = _board->mai_data_bus;
        cb->txfr_len = _snd.slot_words[slot] * sizeof(uint32_t);
        cb->stride = 0x00;
        cb->nextconbk = 0x00;
        cb->null1 = 0x00;
        cb->null2 = 0x00;
        _snd.slot_state[slot] = DMA_SLOT_ACTIVE;
        _snd.active_tail_end_usec += audio_dma_samples_usec(_snd.slot_words[slot]);
        _snd.active_slots[_snd.active_count] = slot;
        _snd.active_end_usec[_snd.active_count] = _snd.active_tail_end_usec;
        _snd.active_count++;
        *samples += _snd.slot_words[slot];
        if (*head_slot == DMA_SLOT_INVALID) {
            *head_slot = slot;
        }
        if (tail_slot != DMA_SLOT_INVALID) {
            dma_cb_t* tail_cb = audio_slot_dma_cb(tail_slot);
            if (tail_cb != NULL) {
                tail_cb->nextconbk = (uint32_t)audio_slot_dma_cb_bus(slot);
            }
        }
        tail_slot = slot;
    }
    return (*head_slot != DMA_SLOT_INVALID) && (*samples > 0);
}

static int audio_start_dma_transfer(uint32_t slot, uint32_t samples, bool is_rebuffer_start) {
    volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
    volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;
    uint32_t dma_enable_bits;
    ewokos_addr_t cb_bus;

    if (samples == 0 || slot >= DMA_BUFFER_SLOTS) {
        return 0;
    }
    cb_bus = audio_slot_dma_cb_bus(slot);
    *(dma + DMA_CS) = DMA_RESET;
    (void)*(dma + DMA_CS);
    dma_enable_bits = *dmae;
    *dmae = dma_enable_bits | DMA_ENABLE_BIT;
    *(dma + DMA_CONBLK_AD) = (uint32_t)cb_bus;
    *(dma + DMA_CS) = DMA_ACTIVE | DMA_PRIORITY_DEFAULT | DMA_PANIC_PRIORITY_DEFAULT;
    _snd.dma_started_usec = audio_now_usec();
    _snd.dma_expected_usec = audio_dma_watchdog_usec(samples);
    _snd.dma_running = true;
    if (is_rebuffer_start) {
        _snd.rebuffer_restart_count++;
    }
    return 0;
}

static bool audio_force_recover_stall(uint32_t now_usec) {
    uint32_t slot = DMA_SLOT_INVALID;
    uint32_t samples = 0;
    bool rebuffer_start;
    uint32_t pending_words;
    uint32_t ring_words = 0;
    uint32_t total_pending_words;

    UNUSED(now_usec);
    if (_snd.dma_running) {
        if (!audio_dma_active() ||
                (_snd.active_count > 0 &&
                 audio_dma_current_active_slot() == DMA_SLOT_INVALID)) {
            _snd.dma_running = false;
            _snd.dma_started_usec = 0;
            _snd.dma_expected_usec = 0;
            audio_queue_complete_active_chain();
            if (audio_queue_pending_words() == 0) {
                _snd.need_rebuffer = true;
            }
        }
    }
    if (_snd.dma_running) {
        return false;
    }
    pending_words = audio_queue_pending_words();
    if (_snd.frame_bytes != 0) {
        ring_words = (audio_pcm_ring_pending_bytes() / _snd.frame_bytes) *
                audio_output_words_per_frame();
    }
    total_pending_words = pending_words + ring_words;
    if (ring_words != 0 &&
            total_pending_words < audio_queue_rebuffer_words_threshold()) {
        _snd.need_rebuffer = true;
        return false;
    }
    if (audio_queue_force_finalize_fill_slot()) {
        /* padded tail with silence */
    }
    if (audio_queue_pending_words() == 0) {
        return false;
    }
    rebuffer_start = _snd.need_rebuffer;
    _snd.need_rebuffer = false;
    pending_words = audio_queue_pending_words();
    if (audio_pcm_ring_pending_bytes() == 0 &&
            pending_words < audio_queue_rebuffer_words_threshold()) {
        _snd.need_rebuffer = true;
        return false;
    }
    if (!audio_queue_prepare_dma_chain(now_usec, &slot, &samples) || samples == 0) {
        _snd.need_rebuffer = rebuffer_start;
        return false;
    }
    audio_start_dma_transfer(slot, samples, rebuffer_start);
    return true;
}

static void audio_service_locked(uint32_t now_usec, bool* wake_writer,
        bool* start_dma, bool* rebuffer_start, uint32_t* slot, uint32_t* samples) {
    bool stalled_no_dma = false;
    bool stalled_active_lost = false;

    if (!_snd.started) {
        return;
    }
    if (_snd.dma_running && !audio_dma_active()) {
        _snd.dma_running = false;
        _snd.dma_started_usec = 0;
        _snd.dma_expected_usec = 0;
        audio_queue_complete_active_chain();
        if (audio_queue_pending_words() == 0) {
            _snd.need_rebuffer = true;
        }
        *wake_writer = true;
    }
    else if (_snd.dma_running &&
            _snd.dma_started_usec != 0 &&
            audio_elapsed_usec(_snd.dma_started_usec, now_usec) > _snd.dma_expected_usec) {
        _snd.dma_watchdog_count++;
        _snd.dma_running = false;
        _snd.dma_started_usec = 0;
        _snd.dma_expected_usec = 0;
        audio_queue_complete_active_chain();
        if (audio_queue_pending_words() == 0) {
            _snd.need_rebuffer = true;
        }
        *wake_writer = true;
    }
    else if (_snd.dma_running) {
        uint32_t appended_samples;

        if (audio_queue_release_scheduled_active(now_usec)) {
            *wake_writer = true;
        }
        appended_samples = audio_queue_append_ready_chain(now_usec);
        if (appended_samples != 0) {
            _snd.dma_expected_usec += audio_dma_samples_usec(appended_samples);
            *wake_writer = true;
        }
    }

    if (!_snd.dma_running && audio_queue_pending_words() > 0 && *samples == 0) {
        if (_snd.need_rebuffer &&
                audio_queue_pending_words() < audio_queue_rebuffer_words_threshold()) {
            /* let the watchdog path below break a stalled low-fill state */
        }
        else if (audio_queue_prepare_dma_chain(now_usec, slot, samples)) {
            *rebuffer_start = _snd.need_rebuffer;
            _snd.need_rebuffer = false;
            *start_dma = true;
            *wake_writer = true;
        }
    }

    if (_snd.last_push_usec != 0 &&
            audio_elapsed_usec(_snd.last_push_usec, now_usec) >= DMA_START_IDLE_FLUSH_US &&
            audio_queue_pending_words() > 0) {
        stalled_no_dma = !_snd.dma_running;
        stalled_active_lost = _snd.dma_running &&
                _snd.active_count > 0 &&
                audio_dma_current_active_slot() == DMA_SLOT_INVALID;
        if ((stalled_no_dma || stalled_active_lost) &&
                audio_force_recover_stall(now_usec)) {
            *wake_writer = true;
        }
    }
}

/* ---- HDMI MAI programming + PCM lifecycle ---------------------------- */

static void hdmi_write_audio_infoframe(unsigned int channels, int rate, int bit_depth) {
    uint8_t buffer[VC4_HDMI_PACKET_STRIDE];
    uint32_t packet_id = HDMI_AUDIO_INFOFRAME_PACKET_ID;
    uint32_t packet_reg = _board->off_ram_packet_start + VC4_HDMI_PACKET_STRIDE * packet_id;
    uint32_t packet_reg_next = _board->off_ram_packet_start +
            VC4_HDMI_PACKET_STRIDE * (packet_id + 1);
    uint32_t freq = sample_rate_to_infoframe_freq(rate);
    uint32_t size = bit_depth_to_infoframe_size(bit_depth);
    uint8_t chan_field = (channels >= 2) ? (uint8_t)(channels - 1) : 0;
    int len = 14;  /* 4-byte header + 10-byte audio infoframe payload */
    uint8_t csum = 0;
    int i;
    int wait;

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = (uint8_t)HDMI_AUDIO_INFOFRAME_TYPE;  /* 0x84 */
    buffer[1] = 1;                                   /* version */
    buffer[2] = 10;                                  /* payload length */
    buffer[3] = 0;                                   /* checksum (filled below) */
    buffer[4] = (uint8_t)((0u << 4) | (chan_field & 0x7));       /* coding=STREAM */
    buffer[5] = (uint8_t)(((freq & 0x7) << 2) | (size & 0x3));
    buffer[6] = 0;   /* coding_type_ext */
    buffer[7] = 0;   /* channel_allocation */
    buffer[8] = 0;   /* level_shift / downmix_inhibit */

    for (i = 0; i < len; i++) {
        csum = (uint8_t)(csum + buffer[i]);
    }
    buffer[3] = (uint8_t)(256 - csum);

    /* Stop the audio infoframe packet and wait for it to go idle. */
    core_wr(_board->off_ram_packet_config,
            core_rd(_board->off_ram_packet_config) & ~(1u << packet_id));
    for (wait = 0; wait < 100 &&
            (core_rd(_board->off_ram_packet_status) & (1u << packet_id)); wait++) {
        usleep(1000);
    }

    /* The packet RAM must be enabled before storing a packet. */
    core_wr(_board->off_ram_packet_config,
            core_rd(_board->off_ram_packet_config) | VC4_HDMI_RAM_PACKET_ENABLE);

    /* Write the packed infoframe into the packet RAM (7 bytes -> 2 words). */
    for (i = 0; i < len; i += 7) {
        hd_wr(_ram_base, packet_reg,
                (uint32_t)buffer[i + 0] |
                ((uint32_t)buffer[i + 1] << 8) |
                ((uint32_t)buffer[i + 2] << 16));
        packet_reg += 4;
        hd_wr(_ram_base, packet_reg,
                (uint32_t)buffer[i + 3] |
                ((uint32_t)buffer[i + 4] << 8) |
                ((uint32_t)buffer[i + 5] << 16) |
                ((uint32_t)buffer[i + 6] << 24));
        packet_reg += 4;
    }
    for (; packet_reg < packet_reg_next; packet_reg += 4) {
        hd_wr(_ram_base, packet_reg, 0);
    }

    /* Re-enable the audio infoframe packet and wait for it to start. */
    core_wr(_board->off_ram_packet_config,
            core_rd(_board->off_ram_packet_config) | (1u << packet_id));
    for (wait = 0; wait < 100 &&
            !(core_rd(_board->off_ram_packet_status) & (1u << packet_id)); wait++) {
        usleep(1000);
    }
}

static void hdmi_mai_program(const struct pcm_config* cfg) {
    unsigned int channels = HDMI_OUTPUT_CHANNELS;
    uint32_t channel_mask = (channels >= 2) ? 0x3u : 0x1u;
    uint32_t channel_map = 0;
    uint32_t mai_fmt;
    uint32_t mai_thr;
    uint32_t audio_packet_config;
    uint32_t m_ctl;
    unsigned int i;

    /* Make sure the audio master (M block) is running; firmware normally
     * enables it, but asserting the enable bit (never SW_RST) is safe and
     * keeps audio flowing if it was left off. */
    m_ctl = hdreg_rd(_board->off_m_ctl);
    if (!(m_ctl & VC4_HD_M_ENABLE)) {
        hdreg_wr(_board->off_m_ctl, m_ctl | VC4_HD_M_ENABLE);
    }

    /* startup: reset + flush the MAI. */
    hdreg_wr(_board->off_mai_ctl,
            VC4_HD_MAI_CTL_RESET | VC4_HD_MAI_CTL_FLUSH | VC4_HD_MAI_CTL_DLATE |
            VC4_HD_MAI_CTL_ERRORE | VC4_HD_MAI_CTL_ERRORF);
    usleep(2000);

    /* MAI sampling clock divider (N/M). */
    audio_set_mai_clock((unsigned int)cfg->rate, _snd.pixel_clock_hz);

    /* MAI control: channel count, whole-sample, channel-align, enable. */
    hdreg_wr(_board->off_mai_ctl,
            ((channels << VC4_HD_MAI_CTL_CHNUM_SHIFT) & VC4_HD_MAI_CTL_CHNUM_MASK) |
            VC4_HD_MAI_CTL_WHOLSMP | VC4_HD_MAI_CTL_CHALIGN | VC4_HD_MAI_CTL_ENABLE);

    /* MAI format: PCM + sample rate. */
    mai_fmt = ((sample_rate_to_mai_fmt(cfg->rate) << VC4_HDMI_MAI_FORMAT_SAMPLE_RATE_SHIFT) &
               (0xffu << VC4_HDMI_MAI_FORMAT_SAMPLE_RATE_SHIFT)) |
              ((VC4_HDMI_MAI_FORMAT_PCM << VC4_HDMI_MAI_FORMAT_AUDIO_FORMAT_SHIFT) &
               (0xffu << VC4_HDMI_MAI_FORMAT_AUDIO_FORMAT_SHIFT));
    hdreg_wr(_board->off_mai_fmt, mai_fmt);

    /*
     * MAI FIFO / DMA thresholds. This matches the Linux vc4_hdmi non-D0 gen5
     * branch (VC4_HD_MAI_THR_* field positions) used on BCM2711B0 silicon.
     * BCM2711C0/D0 steppings widen the fields to 7 bits at the
     * VC4_D0_HD_MAI_THR_* positions; EwokOS userspace cannot read the silicon
     * stepping, so we target the B0 layout. If a C0/D0 Pi4 shows crackling,
     * switch these four fields to the VC4_D0_HD_MAI_THR_* shifts.
     */
    if (_board->mai_thr_gen5) {
        mai_thr = (0x10u << VC4_HD_MAI_THR_PANICHIGH_SHIFT) |
                  (0x10u << VC4_HD_MAI_THR_PANICLOW_SHIFT) |
                  (0x1cu << VC4_HD_MAI_THR_DREQHIGH_SHIFT) |
                  (0x1cu << VC4_HD_MAI_THR_DREQLOW_SHIFT);
    }
    else {
        mai_thr = (0x8u << VC4_HD_MAI_THR_PANICHIGH_SHIFT) |
                  (0x8u << VC4_HD_MAI_THR_PANICLOW_SHIFT) |
                  (0x6u << VC4_HD_MAI_THR_DREQHIGH_SHIFT) |
                  (0x8u << VC4_HD_MAI_THR_DREQLOW_SHIFT);
    }
    hdreg_wr(_board->off_mai_thr, mai_thr);

    /* MAI config: bit/format reverse + active channel mask. */
    core_wr(_board->off_mai_config,
            VC4_HDMI_MAI_CONFIG_BIT_REVERSE | VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE |
            (channel_mask & VC4_HDMI_MAI_CHANNEL_MASK_MASK));

    /* Channel map (vc4 packs 3 bits/channel, vc5 packs 4 bits/channel). */
    for (i = 0; i < 8; i++) {
        if (channel_mask & (1u << i)) {
            channel_map |= _board->channel_map_4bit ? (i << (4 * i)) : (i << (3 * i));
        }
    }
    core_wr(_board->off_mai_channel_map, channel_map);

    /* Audio packet config: B-frame id 8 (matches alsa-lib) + CEA mask. */
    audio_packet_config =
            VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT |
            VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS |
            (0x8u << VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER_SHIFT) |
            (channel_mask & VC4_HDMI_AUDIO_PACKET_CEA_MASK_MASK);
    core_wr(_board->off_audio_packet_config, audio_packet_config);

    /* Audio clock regeneration (N / CTS). */
    audio_set_n_cts((unsigned int)cfg->rate, _snd.pixel_clock_hz);

    /* Audio infoframe so the sink unmutes and decodes the PCM stream. */
    hdmi_write_audio_infoframe(channels, cfg->rate, cfg->bit_depth);
}

static void audio_deinit(void) {
    if (_snd.dma_cbs_addr != 0) {
        dma_free(0, _snd.dma_cbs_addr);
        _snd.dma_cbs_addr = 0;
        _snd.dma_cbs = NULL;
    }
    if (_snd.dma_data_base_addr != 0) {
        dma_free(0, _snd.dma_data_base_addr);
        _snd.dma_data_base_addr = 0;
    }
    if (_snd.pcm_ring != NULL) {
        free(_snd.pcm_ring);
        _snd.pcm_ring = NULL;
    }
    _snd.dma_cbs_phy = 0;
    _snd.dma_data_base_phy = 0;
    _snd.pcm_ring_bytes = 0;
    audio_pcm_ring_reset();
    memset(_snd.dma_slots, 0, sizeof(_snd.dma_slots));
    memset(_snd.dma_slot_phys, 0, sizeof(_snd.dma_slot_phys));
    _snd.frame_bytes = 0;
    _snd.period_bytes = 0;
    _snd.buffer_bytes = 0;
    _snd.write_chunk_bytes = 0;
    audio_queue_reset();
    _snd.dma_started_usec = 0;
    _snd.dma_expected_usec = 0;
    memset(&_snd.pcm_cfg, 0, sizeof(_snd.pcm_cfg));
    _snd.configured = false;
    _snd.prepared = false;
    _snd.started = false;
    _snd.dma_running = false;
}

static int audio_init_pcm(const struct pcm_config *cfg) {
    uint8_t* dma_base;
    uint32_t ring_bytes;
    uint32_t i;

    _snd.pixel_clock_hz = audio_measure_pixel_clock();
    hdmi_mai_program(cfg);

    _snd.dma_cbs_addr = dma_alloc(0, sizeof(dma_cb_t) * DMA_BUFFER_SLOTS);
    _snd.dma_cbs = (dma_cb_t*)(uintptr_t)_snd.dma_cbs_addr;
    _snd.dma_data_base_addr = (ewokos_addr_t)dma_alloc(0, DMA_TOTAL_BUF_SIZE);
    if (_snd.dma_cbs_addr == 0 || _snd.dma_cbs == NULL || _snd.dma_data_base_addr == 0) {
        audio_deinit();
        return -1;
    }
    _snd.dma_data_base_phy = dma_phy_addr(0, _snd.dma_data_base_addr);
    dma_base = (uint8_t*)(uintptr_t)_snd.dma_data_base_addr;
    for (i = 0; i < DMA_BUFFER_SLOTS; i++) {
        _snd.dma_slots[i] = (uint32_t*)(void*)(dma_base + (i * DMA_BUF_SIZE));
        _snd.dma_slot_phys[i] = _snd.dma_data_base_phy + (i * DMA_BUF_SIZE);
    }
    ring_bytes = audio_pcm_ring_capacity_bytes(_snd.frame_bytes);
    _snd.pcm_ring = (uint8_t*)malloc(ring_bytes);
    if (_snd.pcm_ring == NULL) {
        audio_deinit();
        return -1;
    }
    _snd.pcm_ring_bytes = ring_bytes;
    audio_pcm_ring_reset();
    audio_queue_reset();

    _snd.dma_cbs_phy = dma_phy_addr(0, _snd.dma_cbs_addr);
    for (i = 0; i < DMA_BUFFER_SLOTS; i++) {
        _snd.dma_cbs[i].ti = DMA_DEST_DREQ + _board->dma_permap + DMA_SRC_INC;
        _snd.dma_cbs[i].source_ad = (uint32_t)_snd.dma_slot_phys[i] | DMA_VC_ALIAS_UNCACHED;
        _snd.dma_cbs[i].dest_ad = _board->mai_data_bus;
        _snd.dma_cbs[i].txfr_len = 0x00;
        _snd.dma_cbs[i].stride = 0x00;
        _snd.dma_cbs[i].nextconbk = 0x00;
        _snd.dma_cbs[i].null1 = 0x00;
        _snd.dma_cbs[i].null2 = 0x00;
    }
    _snd.configured = true;
    _snd.prepared = false;
    _snd.started = false;
    return 0;
}

static int audio_hw_params(const struct pcm_config *cfg) {
    uint32_t sample_bytes;

    if (cfg->bit_depth != 8 && cfg->bit_depth != 16 &&
            cfg->bit_depth != 24 && cfg->bit_depth != 32) {
        return -1;
    }
    if (cfg->rate < 8000 || cfg->rate > 192000) {
        return -1;
    }
    if (cfg->channels < 1 || cfg->channels > 2) {
        return -1;
    }
    if (cfg->period_size <= 0 || cfg->period_count <= 0) {
        return -1;
    }
    sample_bytes = audio_sample_bytes(cfg->bit_depth);
    if (sample_bytes == 0) {
        return -1;
    }

    audio_stop();
    audio_deinit();

    memcpy(&_snd.pcm_cfg, cfg, sizeof(*cfg));
    /* HDMI MAI always outputs stereo subframes; mono is duplicated upstream. */
    _snd.frame_bytes = (uint32_t)cfg->channels * sample_bytes;
    _snd.period_bytes = (uint32_t)cfg->period_size * _snd.frame_bytes;
    _snd.buffer_bytes = _snd.period_bytes * (uint32_t)cfg->period_count;
    _snd.write_chunk_bytes = _snd.buffer_bytes;
    return audio_init_pcm(cfg);
}

static int audio_ensure_default_config(void) {
    struct pcm_config cfg;

    if (_snd.configured) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.bit_depth = SOUND_DEFAULT_BIT_DEPTH;
    cfg.rate = SOUND_DEFAULT_RATE;
    cfg.channels = SOUND_DEFAULT_CHANNELS;
    cfg.period_size = SOUND_DEFAULT_PERIOD_SIZE;
    cfg.period_count = SOUND_DEFAULT_PERIOD_COUNT;
    cfg.start_threshold = 1;
    cfg.stop_threshold = cfg.period_size * cfg.period_count;
    return audio_hw_params(&cfg);
}

static int audio_prepare(void) {
    if (!_snd.configured) {
        return -1;
    }
    _snd.prepared = true;
    return 0;
}

static int audio_start(void) {
    if (!_snd.prepared) {
        return -1;
    }
    if (_snd.started) {
        return 0;
    }
    /* Re-assert MAI enable (it was set during programming). */
    hdreg_wr(_board->off_mai_ctl, hdreg_rd(_board->off_mai_ctl) | VC4_HD_MAI_CTL_ENABLE);
    _snd.started = true;
    return 0;
}

static int audio_stop(void) {
    if (_snd.started) {
        volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
        volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;

        *(dma + DMA_CS) = DMA_RESET;
        *dmae = (*dmae) & (~DMA_ENABLE_BIT);
        /* Stop the audio infoframe packet and reset/flush the MAI. */
        core_wr(_board->off_ram_packet_config,
                core_rd(_board->off_ram_packet_config) &
                ~(1u << HDMI_AUDIO_INFOFRAME_PACKET_ID));
        hdreg_wr(_board->off_mai_ctl, VC4_HD_MAI_CTL_RESET);
        hdreg_wr(_board->off_mai_ctl, VC4_HD_MAI_CTL_ERRORF);
        hdreg_wr(_board->off_mai_ctl, VC4_HD_MAI_CTL_FLUSH);
        audio_queue_reset();
        audio_pcm_ring_reset();
        _snd.dma_started_usec = 0;
        _snd.dma_expected_usec = 0;
        _snd.dma_running = false;
        _snd.started = false;
    }
    return 0;
}

/* ---- vdevice contract ------------------------------------------------ */

static int sound_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info, int oflag, void *p) {
    UNUSED(dev);
    UNUSED(fd);
    UNUSED(info);
    UNUSED(oflag);
    UNUSED(p);

    from_pid = proc_getpid(from_pid);
    pthread_mutex_lock(&_snd_lock);
    if (_snd.open_count > 0 && _snd.occupied_pid != from_pid) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    audio_stop();
    audio_deinit();
    _snd.occupied_pid = from_pid;
    _snd.open_count++;
    pthread_mutex_unlock(&_snd_lock);
    return 0;
}

static int sound_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t *info, void *p) {
    UNUSED(dev);
    UNUSED(fd);
    UNUSED(node);
    UNUSED(info);
    UNUSED(p);

    from_pid = proc_getpid(from_pid);
    pthread_mutex_lock(&_snd_lock);
    if (_snd.occupied_pid != from_pid || _snd.open_count <= 0) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    _snd.open_count--;
    if (_snd.open_count > 0) {
        pthread_mutex_unlock(&_snd_lock);
        return 0;
    }
    audio_stop();
    audio_deinit();
    _snd.occupied_pid = 0;
    pthread_mutex_unlock(&_snd_lock);
    return 0;
}

static int sound_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t *node,
                       const void *buf, int size, off_t offset, void *p) {
    UNUSED(dev);
    UNUSED(fd);
    UNUSED(node);
    UNUSED(offset);
    UNUSED(p);

    const uint8_t *src;
    int total = 0;
    uint32_t consumed;

    from_pid = proc_getpid(from_pid);
    if (size <= 0) {
        return -1;
    }
    pthread_mutex_lock(&_snd_lock);
    if (_snd.occupied_pid != from_pid) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    if (!_snd.configured && audio_ensure_default_config() != 0) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    if (!_snd.prepared) {
        if (audio_prepare() != 0) {
            pthread_mutex_unlock(&_snd_lock);
            return -1;
        }
    }
    if (!_snd.started) {
        if (audio_start() != 0) {
            pthread_mutex_unlock(&_snd_lock);
            return -1;
        }
    }
    if (_snd.frame_bytes == 0) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    pthread_mutex_unlock(&_snd_lock);

    size = (size / (int)_snd.frame_bytes) * (int)_snd.frame_bytes;
    if (size == 0) {
        return 0;
    }
    src = (const uint8_t *)buf;
    while (total < size) {
        pthread_mutex_lock(&_snd_lock);
        consumed = (uint32_t)(size - total);
        if (audio_pcm_ring_avail_bytes() < consumed) {
            consumed = audio_pcm_ring_avail_bytes();
        }
        consumed = (consumed / _snd.frame_bytes) * _snd.frame_bytes;
        if (consumed != 0) {
            consumed = audio_pcm_ring_write_bytes(src + total, consumed);
            total += (int)consumed;
            pthread_mutex_unlock(&_snd_lock);
            continue;
        }
        pthread_mutex_unlock(&_snd_lock);
        break;
    }

    /*
     * Never sleep in the IPC handler: when the ring is full return
     * VFS_ERR_RETRY so the client libc blocks on VFS_EVT_WR and the feeder
     * thread wakes it after drain (same contract as the analog soundd).
     */
    if (total == 0) {
        _snd_writer_parked = true;
        return VFS_ERR_RETRY;
    }
    return total;
}

static uint32_t sound_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
    UNUSED(dev);
    UNUSED(fd);
    UNUSED(from_pid);
    UNUSED(info);
    UNUSED(p);

    pthread_mutex_lock(&_snd_lock);
    if (_snd.configured && audio_pcm_ring_avail_bytes() >= _snd.frame_bytes) {
        pthread_mutex_unlock(&_snd_lock);
        return VFS_EVT_WR;
    }
    pthread_mutex_unlock(&_snd_lock);
    return 0;
}

static int sound_loop(vdevice_t* dev, void* p) {
    UNUSED(dev);
    UNUSED(p);
    usleep(SOUND_FEED_DEEP_IDLE_SLEEP_US);
    return 0;
}

static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t *in, proto_t *ret, void *p) {
    UNUSED(dev);
    UNUSED(p);

    int result = 0;
    struct pcm_config cfg;

    pthread_mutex_lock(&_snd_lock);
    if (_snd.occupied_pid != proc_getpid(from_pid)) {
        pthread_mutex_unlock(&_snd_lock);
        return -1;
    }
    switch (cmd) {
    case CTRL_PCM_DEV_HW:
        memset(&cfg, 0, sizeof(cfg));
        proto_read_to(in, &cfg, sizeof(cfg));
        result = audio_hw_params(&cfg);
        break;
    case CTRL_PCM_DEV_HW_FREE:
        audio_stop();
        audio_deinit();
        result = 0;
        break;
    case CTRL_PCM_DEV_PRPARE:
        result = audio_prepare();
        break;
    case CTRL_PCM_BUF_AVAIL:
        if (!_snd.configured && audio_ensure_default_config() != 0) {
            result = -1;
        }
        else if (_snd.buffer_bytes == 0 || _snd.write_chunk_bytes == 0) {
            result = -1;
        }
        else {
            result = (int)MIN(MIN(_snd.buffer_bytes, _snd.write_chunk_bytes),
                    audio_pcm_ring_avail_bytes());
        }
        break;
    default:
        result = -1;
        break;
    }
    pthread_mutex_unlock(&_snd_lock);
    PF->addi(ret, result);
    return 0;
}

static bool audio_feed_pcm_ring_locked(void) {
    bool consumed = false;

    while (_snd.started &&
            _snd.pcm_ring != NULL &&
            _snd.frame_bytes != 0 &&
            audio_pcm_ring_pending_bytes() >= _snd.frame_bytes &&
            audio_queue_avail_bytes() >= _snd.frame_bytes) {
        uint32_t chunk_bytes = audio_pcm_ring_contig_read_bytes();
        uint32_t pushed_frames;

        chunk_bytes = (chunk_bytes / _snd.frame_bytes) * _snd.frame_bytes;
        if (chunk_bytes == 0) {
            break;
        }
        if (chunk_bytes > audio_queue_avail_bytes()) {
            chunk_bytes = (audio_queue_avail_bytes() / _snd.frame_bytes) * _snd.frame_bytes;
        }
        if (chunk_bytes == 0) {
            break;
        }
        pushed_frames = audio_queue_push_pcm(_snd.pcm_ring + _snd.pcm_ring_rd, (int)chunk_bytes);
        if (pushed_frames == 0) {
            break;
        }
        audio_pcm_ring_consume_bytes(pushed_frames * _snd.frame_bytes);
        consumed = true;
    }
    if (_snd.started &&
            _snd.dma_running &&
            !consumed &&
            _snd.frame_bytes != 0 &&
            audio_pcm_ring_pending_bytes() < _snd.frame_bytes &&
            audio_queue_pending_words() <= DMA_SAMPLE_CAPACITY) {
        _snd.ring_starve_count++;
    }
    return consumed;
}

static void* sound_feeder_thread(void* arg) {
    UNUSED(arg);

    while (true) {
        bool wake_writer = false;
        bool start_dma = false;
        bool rebuffer_start = false;
        uint32_t slot = DMA_SLOT_INVALID;
        uint32_t samples = 0;
        uint32_t now_usec;
        uint32_t sleep_usec;

        pthread_mutex_lock(&_snd_lock);
        if (_snd.feeder_exit) {
            pthread_mutex_unlock(&_snd_lock);
            break;
        }
        now_usec = audio_now_usec();
        if (_snd.feeder_last_loop_usec != 0 && sound_has_pending_work()) {
            uint32_t loop_gap_usec = audio_elapsed_usec(_snd.feeder_last_loop_usec, now_usec);
            if (loop_gap_usec > (SOUND_FEED_IDLE_SLEEP_US + SOUND_FEED_GUARD_US)) {
                _snd.feeder_delay_count++;
                if (loop_gap_usec > _snd.feeder_delay_max_usec) {
                    _snd.feeder_delay_max_usec = loop_gap_usec;
                }
            }
        }
        _snd.feeder_last_loop_usec = now_usec;
        wake_writer = audio_feed_pcm_ring_locked();
        audio_service_locked(now_usec, &wake_writer, &start_dma, &rebuffer_start,
                &slot, &samples);
        if (start_dma) {
            audio_start_dma_transfer(slot, samples, rebuffer_start);
        }
        if (_snd_writer_parked &&
                (!_snd.started ||
                 (_snd.frame_bytes != 0 &&
                  audio_pcm_ring_avail_bytes() >= _snd.frame_bytes))) {
            wake_writer = true;
        }
        if (wake_writer) {
            _snd_writer_parked = false;
        }
        sleep_usec = sound_feeder_sleep_usec(now_usec);
        pthread_mutex_unlock(&_snd_lock);

        if (wake_writer && _snd_dev != NULL) {
            vfs_wakeup(_snd_dev->mnt_info.node, VFS_EVT_WR);
        }
        usleep(sleep_usec);
    }
    return NULL;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/sound0";
    vdevice_t dev;

    _mmio_base = mmio_map();
    memset(&_sys_info, 0, sizeof(_sys_info));
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&_sys_info);
    audio_detect_hw_config();
    pthread_mutex_init(&_snd_lock, NULL);

    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "bcm283x-hdmi-snd");
    dev.open = sound_open;
    dev.close = sound_close;
    dev.write = sound_write;
    dev.dev_cntl = sound_dev_cntl;
    dev.loop_step = sound_loop;
    dev.check_poll_events = sound_check_poll_events;
    _snd_dev = &dev;

    if (!_snd_feeder_started) {
        int err = pthread_create(&_snd_feeder_tid, NULL, sound_feeder_thread, NULL);
        if (err != 0) {
            return 1;
        }
        _snd_feeder_started = true;
    }

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
