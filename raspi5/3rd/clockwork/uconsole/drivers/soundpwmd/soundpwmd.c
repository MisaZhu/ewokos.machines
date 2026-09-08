#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/rp1_audio.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/ipc.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vfs.h>
#include <ewoksys/klog.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define UNUSED(v) ((void)(v))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SOUND_LOG(...) ((void)0)

#define CTRL_PCM_DEV_HW 0xF0
#define CTRL_PCM_DEV_HW_FREE 0xF1
#define CTRL_PCM_DEV_PRPARE 0xF2
#define CTRL_PCM_BUF_AVAIL 0xF3

/*
 * BCM2712 has no BCM283x PWM, clock manager or DMA controller left, so this
 * driver no longer talks to hardware directly: arch/bcm2712/rp1_audio.h
 * drives the RP1 audio_out block (a 2 channel sigma-delta modulator feeding a
 * 40x oversampled, 40 level two-sided PWM) over the RP1 DW AXI DMAC, with
 * GPIO12/13 muxed to the "aaud" function.
 *
 * What that costs us is format freedom. rp1_aout.c only accepts 48 kHz,
 * stereo, S16_LE, because the clock tree is fixed at 153.6 MHz =
 * 48000 * 40 * 80 and the modulator filter leaves ~2.2 dB of headroom. The
 * client ABI is unchanged though: sound_write() still takes whatever
 * struct pcm_config the caller asked for (8/16/24/32 bit, 8..96 kHz, mono or
 * stereo) and the feeder converts to the hardware format on the way into the
 * ring, by linear interpolation for the rate and by the existing S32 path for
 * the bit depth.
 */
#define SOUND_RING_SLOTS        RP1_AUDIO_DEF_SLOTS
#define SOUND_RING_SLOT_FRAMES  RP1_AUDIO_DEF_SLOT_FRAMES

/*
 * The backend treats RP1_AUDIO_GUARD_SLOTS on both sides of the slot the DMAC
 * reports as in flight, so with 16 slots only 11 are ever writable. Filling
 * the other 13 before the channel is enabled is the only chance to get audio
 * into the first lap; leaving it to the feeder would start playback with
 * ~140 ms of silence.
 */
#define SOUND_PRE_FILL_SLOTS \
    (SOUND_RING_SLOTS - RP1_AUDIO_GUARD_SLOTS - 1U)
#define SOUND_PRE_FILL_FRAMES   (SOUND_PRE_FILL_SLOTS * SOUND_RING_SLOT_FRAMES)

#define SOUND_FEED_KICK_SLEEP_US 500U
#define SOUND_FEED_IDLE_SLEEP_US 1000U
/* buffering towards the start target with nothing to publish: poll lazily */
#define SOUND_FEED_WAIT_SLEEP_US 4000U
#define SOUND_FEED_DEEP_IDLE_SLEEP_US 200000U

/* never wait for more than half the PCM ring before starting playback */
#define SOUND_START_TARGET_DIVISOR 2U
/*
 * The DMA ring is free-running, so with no client data it would replay
 * silence forever and hold the amplifier up. Drop it after this long idle;
 * the next write goes through the start-target path again.
 */
#define SOUND_DMA_IDLE_STOP_US 2000000U

#define SOUND_PCM_RING_MIN_BYTES (128U * 1024U)
#define SOUND_PCM_RING_MAX_BYTES (512U * 1024U)
#define SOUND_PCM_RING_BUFFER_MULTIPLIER 8U

#define SOUND_DEFAULT_BIT_DEPTH 16
#define SOUND_DEFAULT_RATE 48000
#define SOUND_DEFAULT_CHANNELS 2
#define SOUND_DEFAULT_PERIOD_SIZE 1024
#define SOUND_DEFAULT_PERIOD_COUNT 4
#define SOUND_DEFAULT_VOLUME_PCT 70U
#define SOUND_VOLUME_STEP_PCT 5U

/*
 * uConsole CM5 carrier: AUD_PWM0/AUD_PWM1 on GPIO12/13 (muxed by
 * rp1_audio_init()), headphone detect on GPIO10 and the power amplifier
 * enable on GPIO11. Only the last two belong to this driver.
 */
#define SOUNDPWM_GPIO_HP_DETECT 10
#define SOUNDPWM_GPIO_AMP_ENABLE 11

/* resampler phase arithmetic, 16.16 fixed point in units of source frames */
#define RES_FRAC_BITS 16
#define RES_ONE (1U << RES_FRAC_BITS)

/* a client frame is at most 2 channels x 4 bytes */
#define SOUND_MAX_FRAME_BYTES 8U

#define SOUND_CMD_BUF 256

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
    struct pcm_config pcm_cfg;
    uint32_t frame_bytes;
    uint32_t period_bytes;
    uint32_t buffer_bytes;
    uint32_t write_chunk_bytes;
    uint32_t start_target_bytes;

    uint8_t* pcm_ring;
    uint32_t pcm_ring_bytes;
    uint32_t pcm_ring_rd;
    uint32_t pcm_ring_wr;
    uint32_t pcm_ring_used;

    /* client format -> 48 kHz stereo S16 */
    uint32_t src_step;
    uint32_t src_phase;
    int16_t prev_l;
    int16_t prev_r;
    bool resamp_primed;

    uint32_t fill_slot;
    uint32_t last_pcm_usec;

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
/* Set by sound_write when it returns VFS_ERR_RETRY (ring full) so the
 * feeder must re-issue vfs_wakeup even without new drain progress. */
static bool _snd_writer_parked = false;
static bool _snd_amp_enabled = false;
static uint32_t _snd_volume_pct = SOUND_DEFAULT_VOLUME_PCT;

static int audio_stop(void);
static void* sound_feeder_thread(void* arg);

/* ------------------------------------------------------------------- amp */

static void audio_update_amp_state(void) {
    bool hp_inserted = bcm2712_gpio_read(SOUNDPWM_GPIO_HP_DETECT);
    bool amp_enabled = !hp_inserted;

    bcm2712_gpio_write(SOUNDPWM_GPIO_AMP_ENABLE, amp_enabled);
    _snd_amp_enabled = amp_enabled;
}

/* ------------------------------------------------------- sample formats */

static uint32_t audio_sample_bytes(int bit_depth) {
    switch (bit_depth) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 24:
        return 3;
    case 32:
        return 4;
    default:
        return 0;
    }
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

static int16_t audio_clip_s16(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static uint32_t audio_clamp_volume_pct(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint32_t)value;
}

static int16_t audio_apply_gain_s16(int16_t sample) {
    int32_t scaled = (int32_t)sample * (int32_t)_snd_volume_pct;
    return audio_clip_s16(scaled / 100);
}

/*
 * Linear interpolation between two S16 samples. frac is a 16.16 weight that
 * the caller keeps strictly below RES_ONE, so the result always stays between
 * a and b and cannot overflow S16.
 */
static int16_t audio_mix_s16(int16_t a, int16_t b, uint32_t frac) {
    int64_t delta = (int64_t)b - (int64_t)a;
    return (int16_t)((int64_t)a + ((delta * (int64_t)frac) >> RES_FRAC_BITS));
}

/* ------------------------------------------------------------------ time */

static uint32_t audio_now_usec(void) {
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)(uint32_t)tv.tv_sec * 1000000ULL) +
            (uint64_t)(uint32_t)tv.tv_usec);
}

static uint32_t audio_elapsed_usec(uint32_t start_usec, uint32_t now_usec) {
    return now_usec - start_usec;
}

/* -------------------------------------------------------------- pcm ring */

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

/*
 * Read the idx-th unread frame without moving the read pointer, converted to
 * stereo S16 with the current volume applied: 8/24/32 bit sources go through
 * the S32 path and mono is duplicated to both channels. The resampler needs
 * this to look one frame past the one it is about to consume.
 */
static bool audio_pcm_ring_peek_frame(uint32_t idx, int16_t* left, int16_t* right) {
    uint8_t frame[SOUND_MAX_FRAME_BYTES];
    uint32_t off;
    uint32_t tail;
    uint32_t sample_bytes;
    int32_t l;
    int32_t r;

    if (_snd.pcm_ring == NULL || _snd.frame_bytes == 0 ||
            _snd.frame_bytes > SOUND_MAX_FRAME_BYTES) {
        return false;
    }
    off = idx * _snd.frame_bytes;
    if (off + _snd.frame_bytes > _snd.pcm_ring_used) {
        return false;
    }

    off = (_snd.pcm_ring_rd + off) % _snd.pcm_ring_bytes;
    /* the ring is a whole number of frames, so a frame spans at most 2 parts */
    tail = _snd.pcm_ring_bytes - off;
    if (tail >= _snd.frame_bytes) {
        memcpy(frame, _snd.pcm_ring + off, _snd.frame_bytes);
    }
    else {
        memcpy(frame, _snd.pcm_ring + off, tail);
        memcpy(frame + tail, _snd.pcm_ring, _snd.frame_bytes - tail);
    }

    sample_bytes = audio_sample_bytes(_snd.pcm_cfg.bit_depth);
    if (sample_bytes == 0) {
        return false;
    }
    if (sample_bytes == 2 && _snd.pcm_cfg.channels == 2) {
        l = (int16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
        r = (int16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
    }
    else {
        l = audio_pcm_sample_to_s32(frame, sample_bytes) >> 16;
        r = (_snd.pcm_cfg.channels > 1)
                ? (audio_pcm_sample_to_s32(frame + sample_bytes, sample_bytes) >> 16)
                : l;
    }

    *left = audio_apply_gain_s16(audio_clip_s16(l));
    *right = audio_apply_gain_s16(audio_clip_s16(r));
    return true;
}

/* ------------------------------------------------------------- resampler */

static void audio_resamp_reset(void) {
    _snd.src_phase = 0;
    _snd.prev_l = 0;
    _snd.prev_r = 0;
    _snd.resamp_primed = false;
}

/* source frames per output frame, 16.16 */
static void audio_resamp_config(void) {
    uint32_t rate = (_snd.pcm_cfg.rate > 0)
            ? (uint32_t)_snd.pcm_cfg.rate : RP1_AUDIO_RATE;
    uint64_t step = ((uint64_t)rate << RES_FRAC_BITS) / RP1_AUDIO_RATE;

    if (step == 0) {
        step = 1;
    }
    _snd.src_step = (uint32_t)step;
}

/*
 * Consume the first source frame into the interpolation history, so the very
 * first output is exactly source frame 0 rather than a mix with silence.
 */
static bool audio_resamp_prime(void) {
    int16_t l;
    int16_t r;

    if (_snd.resamp_primed) {
        return true;
    }
    if (!audio_pcm_ring_peek_frame(0, &l, &r)) {
        return false;
    }
    audio_pcm_ring_consume_bytes(_snd.frame_bytes);
    _snd.prev_l = l;
    _snd.prev_r = r;
    _snd.src_phase = 0;
    _snd.resamp_primed = true;
    return true;
}

/*
 * Emit one 48 kHz stereo frame.
 *
 * The invariant is: _snd.prev_l/r holds source frame n-1, the next unread
 * ring frame is source frame n, and src_phase is the offset of the output
 * position from frame n-1. Interpolating between n-1 and n at that offset is
 * therefore correct as long as src_phase stays below RES_ONE, which the
 * advance loop guarantees by consuming frames.
 *
 * Returns false when the ring ran dry; the state is left untouched so a later
 * call resumes exactly where this one stopped.
 */
static bool audio_resamp_next(int16_t* left, int16_t* right) {
    int16_t nl;
    int16_t nr;

    if (!audio_resamp_prime()) {
        return false;
    }

    while (_snd.src_phase >= RES_ONE) {
        if (!audio_pcm_ring_peek_frame(0, &nl, &nr)) {
            return false;
        }
        _snd.prev_l = nl;
        _snd.prev_r = nr;
        audio_pcm_ring_consume_bytes(_snd.frame_bytes);
        _snd.src_phase -= RES_ONE;
    }

    if (!audio_pcm_ring_peek_frame(0, &nl, &nr)) {
        return false;
    }
    *left = audio_mix_s16(_snd.prev_l, nl, _snd.src_phase);
    *right = audio_mix_s16(_snd.prev_r, nr, _snd.src_phase);
    _snd.src_phase += _snd.src_step;
    return true;
}

/* -------------------------------------------------------------- dma ring */

/*
 * How much PCM has to be buffered before playback is enabled, in client
 * bytes: enough to fill the pre-fill slots completely, so the first lap is
 * real audio. Capped at half the ring so a client that only ever writes a
 * little at a time still starts, and floored at one period so the very first
 * write() of a short stream is not stranded.
 */
static uint32_t audio_start_target_bytes(void) {
    uint64_t src_frames = ((uint64_t)SOUND_PRE_FILL_FRAMES *
            (uint64_t)_snd.src_step) >> RES_FRAC_BITS;
    uint64_t bytes = (src_frames + 2ULL) * (uint64_t)_snd.frame_bytes;
    uint64_t cap = (uint64_t)_snd.pcm_ring_bytes / SOUND_START_TARGET_DIVISOR;

    if (bytes > cap) {
        bytes = cap;
    }
    if (bytes < (uint64_t)_snd.period_bytes) {
        bytes = (uint64_t)_snd.period_bytes;
    }
    if (bytes > (uint64_t)_snd.pcm_ring_bytes) {
        bytes = (uint64_t)_snd.pcm_ring_bytes;
    }
    return (uint32_t)bytes;
}

/*
 * Convert PCM from the ring into one slot of the DMA ring: one 32-bit FIFO
 * word per stereo frame, left in bits [15:0]. Whatever cannot be filled from
 * the ring becomes silence, because the ring is free-running and every slot
 * the DMAC comes back to has to be republished either way.
 */
static uint32_t audio_fill_slot(uint32_t slot) {
    uint32_t* buf = rp1_audio_slot_buffer(slot);
    uint32_t frames = rp1_audio_slot_frames();
    uint32_t filled = 0;

    if (buf == NULL || frames == 0) {
        return 0;
    }
    if (_snd.started) {
        for (; filled < frames; filled++) {
            int16_t l;
            int16_t r;
            if (!audio_resamp_next(&l, &r)) {
                break;
            }
            buf[filled] = rp1_audio_pack_frame(l, r);
        }
    }
    for (uint32_t i = filled; i < frames; i++) {
        buf[i] = 0;
    }
    return filled;
}

/*
 * Republish every slot that became writable since the last pass. Returns the
 * number of audio frames pushed, which is how the feeder tells real drain
 * progress from a ring that is only carrying silence.
 */
static uint32_t audio_service_ring_locked(void) {
    uint32_t slots = rp1_audio_slots();
    uint32_t pushed = 0;

    if (slots == 0 || !_snd.dma_running) {
        return 0;
    }
    /* bounded: one pass must not walk the whole ring while holding the lock */
    for (uint32_t i = 0; i < slots; i++) {
        if (!rp1_audio_slot_writable(_snd.fill_slot)) {
            break;
        }
        pushed += audio_fill_slot(_snd.fill_slot);
        rp1_audio_slot_commit(_snd.fill_slot);
        _snd.fill_slot = (_snd.fill_slot + 1U) % slots;
    }
    return pushed;
}

static int audio_start_ring_locked(void) {
    int ret;

    /*
     * Reuse the ring across an idle stop/restart: the geometry never changes
     * within one pcm_config, and rp1_audio_start() rebuilds every descriptor
     * and resets CH_LLP to slot 0 anyway.
     */
    if (rp1_audio_slots() != SOUND_RING_SLOTS ||
            rp1_audio_slot_frames() != SOUND_RING_SLOT_FRAMES) {
        ret = rp1_audio_setup_ring(SOUND_RING_SLOTS, SOUND_RING_SLOT_FRAMES);
        if (ret != RP1_AUDIO_ERR_NONE) {
            klog("soundpwm: rp1 ring setup failed %d\n", ret);
            return -1;
        }
    }
    audio_resamp_reset();

    /*
     * slot_writable() needs a live CH_LLP, so this pre-fill is the only
     * opportunity to load the first lap before the channel runs. Start the
     * feeder cursor just behind the guard band: with the DMAC on slot 0 that
     * makes slot SOUND_PRE_FILL_SLOTS the first writable one.
     */
    for (uint32_t i = 0; i < SOUND_PRE_FILL_SLOTS; i++) {
        audio_fill_slot(i);
    }
    _snd.fill_slot = SOUND_PRE_FILL_SLOTS;

    ret = rp1_audio_start();
    if (ret != RP1_AUDIO_ERR_NONE) {
        klog("soundpwm: rp1 start failed %d\n", ret);
        rp1_audio_teardown_ring();
        return -1;
    }
    _snd.dma_running = true;
    _snd.last_pcm_usec = audio_now_usec();
    return 0;
}

static void audio_stop_ring_locked(void) {
    rp1_audio_stop();
    audio_resamp_reset();
    _snd.fill_slot = 0;
    _snd.dma_running = false;
}

/* ---------------------------------------------------------- device state */

static void audio_deinit(void) {
    rp1_audio_stop();
    rp1_audio_teardown_ring();

    if (_snd.pcm_ring != NULL) {
        free(_snd.pcm_ring);
        _snd.pcm_ring = NULL;
    }
    _snd.pcm_ring_bytes = 0;
    audio_pcm_ring_reset();
    audio_resamp_reset();

    _snd.frame_bytes = 0;
    _snd.period_bytes = 0;
    _snd.buffer_bytes = 0;
    _snd.write_chunk_bytes = 0;
    _snd.start_target_bytes = 0;
    _snd.src_step = 0;
    _snd.fill_slot = 0;
    _snd.last_pcm_usec = 0;
    memset(&_snd.pcm_cfg, 0, sizeof(_snd.pcm_cfg));
    _snd.configured = false;
    _snd.prepared = false;
    _snd.started = false;
    _snd.dma_running = false;
}

static int audio_init_pcm(void) {
    uint32_t ring_bytes = audio_pcm_ring_capacity_bytes(_snd.frame_bytes);

    _snd.pcm_ring = (uint8_t*)malloc(ring_bytes);
    if (_snd.pcm_ring == NULL) {
        audio_deinit();
        return -1;
    }
    _snd.pcm_ring_bytes = ring_bytes;
    audio_pcm_ring_reset();
    audio_resamp_config();
    audio_resamp_reset();
    _snd.start_target_bytes = audio_start_target_bytes();
    _snd.fill_slot = 0;

    _snd.configured = true;
    _snd.prepared = false;
    _snd.started = false;
    return 0;
}

static int audio_hw_params(const struct pcm_config* cfg) {
    uint32_t sample_bytes;

    if (cfg->bit_depth != 8 && cfg->bit_depth != 16 &&
            cfg->bit_depth != 24 && cfg->bit_depth != 32) {
        SOUND_LOG("soundpwm: unsupported bit depth: %d\n", cfg->bit_depth);
        return -1;
    }
    if (cfg->rate < 8000 || cfg->rate > 96000) {
        SOUND_LOG("soundpwm: unsupported rate: %d\n", cfg->rate);
        return -1;
    }
    if (cfg->channels < 1 || cfg->channels > 2) {
        SOUND_LOG("soundpwm: unsupported channels: %d\n", cfg->channels);
        return -1;
    }
    if (cfg->period_size <= 0 || cfg->period_count <= 0) {
        SOUND_LOG("soundpwm: invalid period config: %d x %d\n",
                cfg->period_size, cfg->period_count);
        return -1;
    }

    sample_bytes = audio_sample_bytes(cfg->bit_depth);
    if (sample_bytes == 0) {
        return -1;
    }

    audio_stop();
    audio_deinit();

    memcpy(&_snd.pcm_cfg, cfg, sizeof(*cfg));
    _snd.frame_bytes = (uint32_t)cfg->channels * sample_bytes;
    _snd.period_bytes = (uint32_t)cfg->period_size * _snd.frame_bytes;
    _snd.buffer_bytes = _snd.period_bytes * (uint32_t)cfg->period_count;
    /* Let user space refill as much free queue as possible per write call. */
    _snd.write_chunk_bytes = _snd.buffer_bytes;
    return audio_init_pcm();
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

/*
 * Mark the stream running. This touches no hardware: the feeder enables the
 * DMAC once start_target_bytes of PCM have accumulated, which is what keeps
 * the first lap free of silence. sound_write() calls this in IPC context, so
 * it must stay cheap and must not sleep.
 */
static int audio_start(void) {
    if (!_snd.prepared) {
        return -1;
    }
    if (_snd.started) {
        return 0;
    }
    audio_pcm_ring_reset();
    audio_resamp_reset();
    _snd.fill_slot = 0;
    _snd.dma_running = false;
    _snd.last_pcm_usec = audio_now_usec();
    _snd.started = true;
    return 0;
}

static int audio_stop(void) {
    if (_snd.started) {
        audio_stop_ring_locked();
        audio_pcm_ring_reset();
        _snd.started = false;
    }
    return 0;
}

/* ---------------------------------------------------------------- feeder */

static uint32_t sound_feeder_sleep_usec(void) {
    if (_snd.open_count <= 0 && !_snd.configured && !_snd.prepared &&
            !_snd.started && !_snd.dma_running &&
            audio_pcm_ring_pending_bytes() == 0) {
        return SOUND_FEED_DEEP_IDLE_SLEEP_US;
    }
    /*
     * A slot is 10.67 ms and the guard band leaves ~117 ms of slack, so a
     * 1 ms poll is comfortably inside budget. Poll twice as often while PCM
     * is waiting, to keep the client's write() flowing.
     */
    if (_snd.frame_bytes != 0 &&
            audio_pcm_ring_pending_bytes() >= _snd.frame_bytes) {
        return SOUND_FEED_KICK_SLEEP_US;
    }
    if (_snd.started && !_snd.dma_running) {
        /*
         * Either still buffering towards the start target or idle-stopped:
         * there is no live ring to republish, so the next write() is what
         * matters and a lazy poll is enough.
         */
        return SOUND_FEED_WAIT_SLEEP_US;
    }
    return SOUND_FEED_IDLE_SLEEP_US;
}

/*
 * One feeder pass, with _snd_lock held. *wake_writer is set when the ring
 * actually drained, so a client parked in vfsd on VFS_EVT_WR can be released.
 */
static void sound_feeder_step_locked(bool* wake_writer) {
    uint32_t now_usec;
    uint32_t pushed;

    if (!_snd.started) {
        return;
    }

    if (!_snd.dma_running) {
        if (audio_pcm_ring_pending_bytes() < _snd.start_target_bytes) {
            return;
        }
        if (audio_start_ring_locked() != 0) {
            /* leave the stream up: the next pass retries */
            return;
        }
    }

    now_usec = audio_now_usec();
    pushed = audio_service_ring_locked();
    if (pushed > 0) {
        _snd.last_pcm_usec = now_usec;
        *wake_writer = true;
    }

    /*
     * Underrun: the ring keeps replaying silence. Give up on it after a
     * while so the modulator and the amplifier do not stay up indefinitely;
     * buffered PCM resumes playback through the start-target path.
     */
    if (_snd.last_pcm_usec != 0 &&
            audio_elapsed_usec(_snd.last_pcm_usec, now_usec) >
                    SOUND_DMA_IDLE_STOP_US) {
        audio_stop_ring_locked();
    }
}

static void* sound_feeder_thread(void* arg) {
    UNUSED(arg);

    while (true) {
        bool wake_writer = false;
        uint32_t sleep_usec;

        pthread_mutex_lock(&_snd_lock);
        if (_snd.feeder_exit) {
            pthread_mutex_unlock(&_snd_lock);
            break;
        }

        sound_feeder_step_locked(&wake_writer);
        /*
         * A parked writer (sound_write returned VFS_ERR_RETRY) sleeps in
         * vfsd and depends on us for the wakeup. Re-issue it as soon as
         * the ring has room again, or when the stream left the running
         * state (so the retry can fail fast instead of hanging forever).
         */
        if (_snd_writer_parked &&
                (!_snd.started ||
                 (_snd.frame_bytes != 0 &&
                  audio_pcm_ring_avail_bytes() >= _snd.frame_bytes))) {
            wake_writer = true;
        }
        if (wake_writer) {
            _snd_writer_parked = false;
        }
        sleep_usec = sound_feeder_sleep_usec();
        pthread_mutex_unlock(&_snd_lock);

        if (wake_writer && _snd_dev != NULL) {
            vfs_wakeup(_snd_dev->mnt_info.node, VFS_EVT_WR);
        }
        proc_usleep(sleep_usec);
    }
    return NULL;
}

/* ------------------------------------------------------------ vdevice io */

static int sound_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info, int oflag, void *p) {
    UNUSED(dev);
    UNUSED(fd);
    UNUSED(from_pid);
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
                       const void *buf, int size, int offset, void *p) {
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
     * Never sleep here: this runs in IPC handler context where
     * proc_usleep() degenerates to a yield-spin (the kernel skips the
     * real sleep for tasks with an in-flight IPC), which burns CPU and
     * blocks every other IPC to soundpwmd. When the ring is full, hand
     * the waiting over to vfsd: return VFS_ERR_RETRY so the client libc
     * blocks on VFS_EVT_WR and the feeder thread wakes it after drain.
     */
    if (total == 0) {
        _snd_writer_parked = true;
        return VFS_ERR_RETRY;
    }
    return total;
}

static uint32_t sound_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
    UNUSED(fd);
    UNUSED(from_pid);
    UNUSED(info);
    UNUSED(p);

    pthread_mutex_lock(&_snd_lock);
    UNUSED(dev);
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
    audio_update_amp_state();
    proc_usleep(SOUND_FEED_DEEP_IDLE_SLEEP_US);
    return 0;
}

static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t *in, proto_t *ret, void *p) {
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

static char* sound_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    char* ret = (char*)malloc(SOUND_CMD_BUF);
    char* end = NULL;
    long requested = 0;

    UNUSED(dev);
    UNUSED(from_pid);
    UNUSED(p);
    if (ret == NULL) {
        return NULL;
    }
    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        free(ret);
        return NULL;
    }

    if (strcmp(argv[0], "help") == 0) {
        snprintf(ret, SOUND_CMD_BUF,
                "help: show commands\n"
                "status: show stream and dma ring state\n"
                "vol: show current volume\n"
                "vol up|down: adjust volume by %u%%\n"
                "vol <0-100>: set volume percent\n",
                (unsigned)SOUND_VOLUME_STEP_PCT);
        return ret;
    }

    /*
     * The RP1 ring is polled rather than interrupt driven, so this is the
     * only way to see whether the DMAC is still walking it and how far the
     * feeder is ahead of the hardware.
     */
    if (strcmp(argv[0], "status") == 0) {
        pthread_mutex_lock(&_snd_lock);
        snprintf(ret, SOUND_CMD_BUF,
                "cfg=%d %dHz %dch %dbit start_target=%u\n"
                "started=%d dma=%d hw_slot=%d fill_slot=%u pending=%u/%u\n"
                "volume=%u%% amp=%s\n",
                (int)_snd.configured, _snd.pcm_cfg.rate, _snd.pcm_cfg.channels,
                _snd.pcm_cfg.bit_depth, (unsigned)_snd.start_target_bytes,
                (int)_snd.started, (int)_snd.dma_running, rp1_audio_hw_slot(),
                (unsigned)_snd.fill_slot,
                (unsigned)audio_pcm_ring_pending_bytes(),
                (unsigned)_snd.pcm_ring_bytes,
                (unsigned)_snd_volume_pct, _snd_amp_enabled ? "on" : "off");
        pthread_mutex_unlock(&_snd_lock);
        return ret;
    }

    if (strcmp(argv[0], "vol") == 0) {
        pthread_mutex_lock(&_snd_lock);
        if (argc < 2 || argv[1] == NULL) {
            snprintf(ret, SOUND_CMD_BUF, "volume=%u%%\n", (unsigned)_snd_volume_pct);
            pthread_mutex_unlock(&_snd_lock);
            return ret;
        }

        if (strcmp(argv[1], "up") == 0) {
            _snd_volume_pct = audio_clamp_volume_pct((int)_snd_volume_pct + (int)SOUND_VOLUME_STEP_PCT);
            snprintf(ret, SOUND_CMD_BUF, "volume=%u%%\n", (unsigned)_snd_volume_pct);
            pthread_mutex_unlock(&_snd_lock);
            return ret;
        }
        if (strcmp(argv[1], "down") == 0) {
            _snd_volume_pct = audio_clamp_volume_pct((int)_snd_volume_pct - (int)SOUND_VOLUME_STEP_PCT);
            snprintf(ret, SOUND_CMD_BUF, "volume=%u%%\n", (unsigned)_snd_volume_pct);
            pthread_mutex_unlock(&_snd_lock);
            return ret;
        }

        requested = strtol(argv[1], &end, 10);
        if (argv[1][0] == 0 || end == NULL || *end != 0) {
            snprintf(ret, SOUND_CMD_BUF, "usage: vol [up|down|0-100]\n");
            pthread_mutex_unlock(&_snd_lock);
            return ret;
        }
        _snd_volume_pct = audio_clamp_volume_pct((int)requested);
        snprintf(ret, SOUND_CMD_BUF, "volume=%u%%\n", (unsigned)_snd_volume_pct);
        pthread_mutex_unlock(&_snd_lock);
        return ret;
    }

    snprintf(ret, SOUND_CMD_BUF, "unknown command: %s\ntry: help\n", argv[0]);
    return ret;
}

/* ------------------------------------------------------------------ main */

static void audio_hw_init(void) {
    /*
     * GPIO12/13 are muxed to the RP1 audio_out function by rp1_audio_init(),
     * so only the headphone detect and amplifier enable pins are ours here.
     * The detect pad keeps whatever pull the RP1 firmware left on it, the
     * same as the bcm283x driver did by not touching GPPUD.
     *
     * This runs BEFORE rp1_audio_init(): the amplifier has to be pinned off
     * first, because as soon as the pins are muxed the modulator drives them
     * with its muted bias level (a 50% duty PWM) and an amplifier that is
     * still floating from the firmware's boot state would turn that into an
     * audible pop. audio_update_amp_state() is what releases it afterwards.
     */
    bcm2712_gpio_config(SOUNDPWM_GPIO_HP_DETECT, GPIO_FUNC_INPUT);
    bcm2712_gpio_config(SOUNDPWM_GPIO_AMP_ENABLE, GPIO_FUNC_OUTPUT);
    bcm2712_gpio_write(SOUNDPWM_GPIO_AMP_ENABLE, false);
    _snd_amp_enabled = false;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/soundpwm0";
    int ret;

    _mmio_base = mmio_map();
    if (_mmio_base == 0) {
        klog("soundpwm: mmio map failed\n");
        return -1;
    }

    bcm2712_gpio_init();
    /* hold the amplifier off before the modulator reaches the pins */
    audio_hw_init();
    /*
     * Audio clock tree, audio_out block, DMAC and the GPIO12/13 "aaud"
     * pinmux. The output comes out of this muted, so it is safe to let the
     * amplifier follow the headphone jack now.
     */
    ret = rp1_audio_init(0);
    if (ret != RP1_AUDIO_ERR_NONE) {
        klog("soundpwm: rp1 audio init failed %d\n", ret);
        return -1;
    }
    audio_update_amp_state();
    pthread_mutex_init(&_snd_lock, NULL);

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "rp1-audio-snd");
    dev.open = sound_open;
    dev.close = sound_close;
    dev.write = sound_write;
    dev.dev_cntl = sound_dev_cntl;
    dev.cmd = sound_dev_cmd;
    dev.loop_step = sound_loop;
    dev.check_poll_events = sound_check_poll_events;
    _snd_dev = &dev;
    if (!_snd_feeder_started) {
        int err = pthread_create(&_snd_feeder_tid, NULL, sound_feeder_thread, NULL);
        if (err != 0) {
            SOUND_LOG("soundpwm: pthread_create failed %d\n", err);
            return 1;
        }
        _snd_feeder_started = true;
    }

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
