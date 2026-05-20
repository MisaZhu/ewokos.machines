#include <arch/bcm283x/gpio.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/ipc.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vfs.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <stdbool.h>
#include <stdint.h>

#define UNUSED(v) ((void)(v))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define CTRL_PCM_DEV_HW 0xF0
#define CTRL_PCM_DEV_HW_FREE 0xF1
#define CTRL_PCM_DEV_PRPARE 0xF2
#define CTRL_PCM_BUF_AVAIL 0xF3

#define PWM_BASE        (_mmio_base + 0x20C000)
#define CLOCK_BASE      (_mmio_base + 0x101000)
#define PWM_BUS_BASE    0x7E20C000u
#define PWM_FIFO_BUS_ADDR (PWM_BUS_BASE + 0x18u)
#define DMA_VC_ALIAS_UNCACHED 0xC0000000u
#define DMA_BUS_ADDR_MASK 0x3FFFFFFFu

#define DMA_CHANNEL     5U
#define DMA_BASE        (_mmio_base + 0x007000 + (DMA_CHANNEL * 0x100u))
#define DMA_ENABLE      (_mmio_base + 0x007FF0)

#define BCM283x_PWMCLK_CNTL 40
#define BCM283x_PWMCLK_DIV  41
#define PM_PASSWORD 0x5A000000

#define BCM283x_PWM_CONTROL 0
#define BCM283x_PWM_STATUS  1
#define BCM283x_PWM_DMAC    2
#define BCM283x_PWM_FIFO    6
#define BCM283x_PWM_RANGE1  4
#define BCM283x_PWM_DATA1   5
#define BCM283x_PWM_RANGE2  8
#define BCM283x_PWM_DATA2   9
#define BCM283x_PWM_PWEN1   (1U << 0)
#define BCM283x_PWM_USEF1   (1U << 5)
#define BCM283x_PWM_CLRF1   (1U << 6)
#define BCM283x_PWM_PWEN2   (1U << 8)
#define BCM283x_PWM_USEF2   (1U << 13)

#define BCM283x_PWM_ENAB      0x80000000

#define BCM283x_GAPO2 0x20
#define BCM283x_GAPO1 0x10
#define BCM283x_RERR1 0x8
#define BCM283x_WERR1 0x4
#define BCM283x_FULL1 0x1
#define ERRORMASK (BCM283x_GAPO2 | BCM283x_GAPO1 | BCM283x_RERR1 | BCM283x_WERR1)

#define DMA_CS        0
#define DMA_CONBLK_AD 1
#define DMA_ENABLE_BIT (1U << DMA_CHANNEL)
#define DMA_ACTIVE    1
#define DMA_RESET     (1U << 31)
#define DMA_PRIORITY_DEFAULT (8U << 16)
#define DMA_PANIC_PRIORITY_DEFAULT (8U << 20)
#define DMA_DEST_DREQ 0x40
#define DMA_SRC_INC   0x100

#define DMA_PERMAP_PWM  (DMA_CHANNEL << 16)

#define DMA_BUF_SIZE  (1024*4)
#define DMA_SAMPLE_CAPACITY (DMA_BUF_SIZE / sizeof(uint32_t))
#define DMA_BUFFER_SLOTS 32U
#define DMA_TOTAL_BUF_SIZE (DMA_BUF_SIZE * DMA_BUFFER_SLOTS)
#define DMA_SLOT_INVALID DMA_BUFFER_SLOTS
#define DMA_CHAIN_START_SLOTS 4U
#define DMA_CHAIN_REBUFFER_START_SLOTS 6U
#define PWM_OUTPUT_CHANNELS 2U
#define PWM_BASE_CLOCK_HZ 100000000U
#define PWM_CLOCK_SOURCE_SELECT 6U
#define PWM_CLOCK_DIV_INT 5U
#define PWM_CLOCK_DIV_FRAC 0U
#define SOUND_ACTIVE_SLEEP_US 100
#define SOUND_IDLE_SLEEP_US 1000
#define SOUND_DEEP_IDLE_SLEEP_US 20000
#define DMA_START_IDLE_FLUSH_US 40000U
#define DMA_WATCHDOG_MARGIN_US 200000U
#define SOUND_DEFAULT_BIT_DEPTH 16
#define SOUND_DEFAULT_RATE 48000
#define SOUND_DEFAULT_CHANNELS 2
#define SOUND_DEFAULT_PERIOD_SIZE 1024
#define SOUND_DEFAULT_PERIOD_COUNT 4
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
	uint32_t pwm_range;
	uint32_t pwm_scale;
	uint32_t frame_bytes;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
	uint32_t write_chunk_bytes;
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
	uint32_t diag_last_log_usec;
	bool need_rebuffer;
	bool configured;
	bool prepared;
	bool started;
	bool dma_running;
	int open_count;
	int occupied_pid;
} snd_dev_t;

static snd_dev_t _snd = {0};

static void sound_pump(vdevice_t* dev);
static int audio_stop(void);
static uint32_t audio_elapsed_usec(uint32_t start_usec, uint32_t now_usec);

#define DMA_SLOT_EMPTY   0U
#define DMA_SLOT_FILLING 1U
#define DMA_SLOT_READY   2U
#define DMA_SLOT_ACTIVE  3U

static uint32_t audio_output_words_per_frame(void) {
	return PWM_OUTPUT_CHANNELS;
}

static uint32_t audio_pwm_control_flags(void) {
	return BCM283x_PWM_PWEN1 | BCM283x_PWM_USEF1 |
			BCM283x_PWM_CLRF1 |
			BCM283x_PWM_PWEN2 | BCM283x_PWM_USEF2;
}

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

static uint32_t audio_queue_pending_words(void) {
	uint32_t words = 0;

	for (uint32_t i = 0; i < DMA_BUFFER_SLOTS; i++) {
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

static bool sound_has_pending_work(void) {
	return _snd.dma_running || audio_queue_pending_words() > 0;
}

static uint32_t sound_poll_sleep_usec(void) {
	if (_snd.open_count <= 0 &&
			!_snd.configured &&
			!_snd.prepared &&
			!_snd.started &&
			!sound_has_pending_work()) {
		return SOUND_DEEP_IDLE_SLEEP_US;
	}
	if (sound_has_pending_work()) {
		return SOUND_ACTIVE_SLEEP_US;
	}
	return SOUND_IDLE_SLEEP_US;
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
	uint32_t start_limit;

	if (_snd.need_rebuffer) {
		if (DMA_CHAIN_REBUFFER_START_SLOTS == 0 ||
				DMA_CHAIN_REBUFFER_START_SLOTS > DMA_BUFFER_SLOTS) {
			return DMA_BUFFER_SLOTS;
		}
		return DMA_CHAIN_REBUFFER_START_SLOTS;
	}
	if (DMA_CHAIN_START_SLOTS == 0 || DMA_CHAIN_START_SLOTS > DMA_BUFFER_SLOTS) {
		start_limit = DMA_BUFFER_SLOTS;
	}
	else {
		start_limit = DMA_CHAIN_START_SLOTS;
	}
	if (_snd.ready_count >= start_limit &&
			DMA_CHAIN_REBUFFER_START_SLOTS > start_limit &&
			DMA_CHAIN_REBUFFER_START_SLOTS <= DMA_BUFFER_SLOTS) {
		return DMA_CHAIN_REBUFFER_START_SLOTS;
	}
	return start_limit;
}

static uint32_t audio_dma_samples_usec(uint32_t samples) {
	uint64_t words_per_frame = audio_output_words_per_frame();

	if (_snd.pcm_cfg.rate <= 0 || samples == 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)samples * 1000000ULL) /
			((uint64_t)_snd.pcm_cfg.rate * words_per_frame));
}

static uint32_t audio_now_usec(void) {
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)(uint32_t)tv.tv_sec * 1000000ULL) +
			(uint64_t)(uint32_t)tv.tv_usec);
}

static uint32_t audio_dma_current_cb_bus(void) {
	volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
	return *(dma + DMA_CONBLK_AD);
}

static void audio_log_diag(const char* reason, uint32_t now_usec, uint32_t cb_bus) {
	if (now_usec != 0 && _snd.diag_last_log_usec != 0 &&
			audio_elapsed_usec(_snd.diag_last_log_usec, now_usec) < 500000U) {
		return;
	}
	_snd.diag_last_log_usec = now_usec;
	slog("sound: %s cb=%x active=%u ready=%u fill=%u pending=%u avail=%u running=%d need_rebuffer=%d\n",
			reason,
			cb_bus,
			_snd.active_count,
			_snd.ready_count,
			_snd.fill_slot,
			audio_queue_pending_words(),
			audio_queue_avail_bytes(),
			_snd.dma_running ? 1 : 0,
			_snd.need_rebuffer ? 1 : 0);
}

static uint32_t audio_elapsed_usec(uint32_t start_usec, uint32_t now_usec) {
	return now_usec - start_usec;
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

static uint32_t audio_rate_to_pwm_range(int rate) {
	uint64_t range;

	if (rate <= 0) {
		return 0;
	}

	range = ((uint64_t)PWM_BASE_CLOCK_HZ + ((uint64_t)rate / 2ULL)) / (uint64_t)rate;
	if (range < 2ULL) {
		range = 2ULL;
	}
	if (range > 0xFFFFULL) {
		range = 0xFFFFULL;
	}
	return (uint32_t)range;
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

static uint32_t audio_sample_to_pwm_word(int32_t sample) {
	uint64_t scaled;

	scaled = (((uint64_t)((int64_t)sample + 2147483648LL)) *
			(uint64_t)_snd.pwm_scale) >> 32;
	if (scaled >= _snd.pwm_range) {
		scaled = _snd.pwm_range - 1U;
	}
	return (uint32_t)scaled;
}

static uint32_t audio_s16_to_pwm_word(int16_t sample) {
	uint32_t shifted = (uint32_t)((int32_t)sample + 32768);
	return (shifted * _snd.pwm_scale) >> 16;
}

static void audio_convert_s16_stereo_frames(const uint8_t* src, uint32_t* dst, uint32_t frames) {
	const uint8_t* p = src;
	uint32_t* q = dst;
	uint32_t i = 0;

	for (; i < frames; i++) {
		int16_t left = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
		int16_t right = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));

		q[0] = audio_s16_to_pwm_word(right);
		q[1] = audio_s16_to_pwm_word(left);
		p += 4;
		q += 2;
	}
}

static void audio_frame_to_pwm_words(const uint8_t* frame, uint32_t* words) {
	uint32_t sample_bytes;
	int32_t left;
	int32_t right;

	if (_snd.pcm_cfg.bit_depth == 16) {
		int16_t left16 = (int16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
		int16_t right16 = left16;

		if (_snd.pcm_cfg.channels > 1) {
			right16 = (int16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
		}
		words[0] = audio_s16_to_pwm_word(right16);
		words[1] = audio_s16_to_pwm_word(left16);
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

	/* The board analog FIFO consumes right/left word order. */
	words[0] = audio_sample_to_pwm_word(right);
	words[1] = audio_sample_to_pwm_word(left);
}

static uint32_t audio_find_empty_slot(void) {
	for (uint32_t i = 0; i < DMA_BUFFER_SLOTS; i++) {
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

	if (slot >= DMA_BUFFER_SLOTS ||
			_snd.slot_state[slot] != DMA_SLOT_FILLING ||
			_snd.slot_words[slot] == 0) {
		return false;
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
			for (uint32_t i = 0; i < chunk_frames; i++) {
				audio_frame_to_pwm_words(buf + ((pushed + i) * _snd.frame_bytes),
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

static void audio_set_pwm_range(uint32_t range) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;

	*(pwm + BCM283x_PWM_RANGE1) = range;
	*(pwm + BCM283x_PWM_RANGE2) = range;
}

static dma_cb_t* audio_slot_dma_cb(uint32_t slot) {
	if (_snd.dma_cbs == NULL || slot >= DMA_BUFFER_SLOTS) {
		return NULL;
	}
	return &_snd.dma_cbs[slot];
}

static uint32_t audio_slot_dma_cb_bus(uint32_t slot) {
	return (uint32_t)(_snd.dma_cbs_phy +
			(ewokos_addr_t)(slot * sizeof(dma_cb_t))) | DMA_VC_ALIAS_UNCACHED;
}

static uint32_t audio_dma_cb_bus_normalize(uint32_t cb_bus) {
	return cb_bus & DMA_BUS_ADDR_MASK;
}

static void audio_queue_complete_active_chain(void) {
	for (uint32_t i = 0; i < _snd.active_count; i++) {
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

	for (uint32_t i = 0; i < _snd.active_count; i++) {
		uint32_t slot = _snd.active_slots[i];
		if (slot >= DMA_BUFFER_SLOTS) {
			continue;
		}
		if (audio_dma_cb_bus_normalize(audio_slot_dma_cb_bus(slot)) == cb_bus_norm) {
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
		audio_log_diag("active slot lost", now_usec, audio_dma_current_cb_bus());
		return false;
	}

	while (_snd.active_count > 0 && current_active_idx > 0) {
		uint32_t slot = _snd.active_slots[0];

		_snd.slot_words[slot] = 0;
		_snd.slot_state[slot] = DMA_SLOT_EMPTY;
		for (uint32_t i = 1; i < _snd.active_count; i++) {
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
		cb->dest_ad = PWM_FIFO_BUS_ADDR;
		cb->txfr_len = _snd.slot_words[slot] * sizeof(uint32_t);
		cb->stride = 0x00;
		cb->nextconbk = 0x00;
		cb->null1 = 0x00;
		cb->null2 = 0x00;
		tail_cb->nextconbk = audio_slot_dma_cb_bus(slot);
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

static void audio_try_append_running_chain(uint32_t now_usec) {
	uint32_t appended_samples;

	if (!_snd.dma_running) {
		return;
	}
	audio_queue_release_scheduled_active(now_usec);
	appended_samples = audio_queue_append_ready_chain(now_usec);
	if (appended_samples != 0) {
		_snd.dma_expected_usec += audio_dma_samples_usec(appended_samples);
	}
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
		cb->dest_ad = PWM_FIFO_BUS_ADDR;
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
				tail_cb->nextconbk = audio_slot_dma_cb_bus(slot);
			}
		}
		tail_slot = slot;
	}
	return (*head_slot != DMA_SLOT_INVALID) && (*samples > 0);
}

static int audio_start_dma_transfer(uint32_t slot, uint32_t samples, bool is_rebuffer_start) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
	volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
	volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;
	uint32_t dma_enable_bits;
	uint32_t cb_bus;

	if (samples == 0 || slot >= DMA_BUFFER_SLOTS) {
		return 0;
	}

	cb_bus = audio_slot_dma_cb_bus(slot);
	*(pwm + BCM283x_PWM_STATUS) = ERRORMASK;
	*(dma + DMA_CS) = DMA_RESET;
	(void)*(dma + DMA_CS);
	dma_enable_bits = *dmae;
	*dmae = dma_enable_bits | DMA_ENABLE_BIT;
	*(dma + DMA_CONBLK_AD) = cb_bus;
	*(dma + DMA_CS) = DMA_ACTIVE | DMA_PRIORITY_DEFAULT | DMA_PANIC_PRIORITY_DEFAULT;
	_snd.dma_started_usec = audio_now_usec();
	_snd.dma_expected_usec = audio_dma_watchdog_usec(samples);
	_snd.dma_running = true;
	UNUSED(is_rebuffer_start);
	return 0;
}

static bool audio_force_recover_stall(uint32_t now_usec) {
	uint32_t slot = DMA_SLOT_INVALID;
	uint32_t samples = 0;
	bool rebuffer_start;

	if (_snd.dma_running) {
		if (!audio_dma_active() ||
				(_snd.active_count > 0 &&
				 audio_dma_current_active_slot() == DMA_SLOT_INVALID)) {
			audio_log_diag("force drop active", now_usec, audio_dma_current_cb_bus());
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

	if (audio_queue_force_finalize_fill_slot()) {
		audio_log_diag("force finalize fill", now_usec, audio_dma_current_cb_bus());
	}

	if (audio_queue_pending_words() == 0) {
		return false;
	}

	rebuffer_start = _snd.need_rebuffer;
	_snd.need_rebuffer = false;
	if (!audio_queue_prepare_dma_chain(now_usec, &slot, &samples) || samples == 0) {
		_snd.need_rebuffer = rebuffer_start;
		return false;
	}

	audio_log_diag("force restart dma", now_usec, audio_dma_current_cb_bus());
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
		audio_log_diag("dma watchdog", now_usec, audio_dma_current_cb_bus());
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
			/* Fall through so the watchdog path below can break a stalled low-fill state. */
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
	_snd.dma_cbs_phy = 0;
	_snd.dma_data_base_phy = 0;
	memset(_snd.dma_slots, 0, sizeof(_snd.dma_slots));
	memset(_snd.dma_slot_phys, 0, sizeof(_snd.dma_slot_phys));
	_snd.pwm_range = 0;
	_snd.pwm_scale = 0;
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
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
	uint8_t* dma_base;

	*(pwm + BCM283x_PWM_CONTROL) = 0;
	proc_usleep(2000);
	_snd.pwm_range = audio_rate_to_pwm_range(cfg->rate);
	_snd.pwm_scale = (_snd.pwm_range > 0) ? (_snd.pwm_range - 1U) : 0;
	audio_set_pwm_range(_snd.pwm_range);
	*(pwm + BCM283x_PWM_CONTROL) = audio_pwm_control_flags();

	_snd.dma_cbs_addr = dma_alloc(0, sizeof(dma_cb_t) * DMA_BUFFER_SLOTS);
	_snd.dma_cbs = (dma_cb_t*)(uintptr_t)_snd.dma_cbs_addr;
	_snd.dma_data_base_addr = (ewokos_addr_t)dma_alloc(0, DMA_TOTAL_BUF_SIZE);

	if (_snd.dma_cbs_addr == 0 || _snd.dma_cbs == NULL || _snd.dma_data_base_addr == 0) {
		audio_deinit();
		return -1;
	}

	_snd.dma_data_base_phy = dma_phy_addr(0, _snd.dma_data_base_addr);
	dma_base = (uint8_t*)(uintptr_t)_snd.dma_data_base_addr;
	for (uint32_t i = 0; i < DMA_BUFFER_SLOTS; i++) {
		_snd.dma_slots[i] = (uint32_t*)(void*)(dma_base + (i * DMA_BUF_SIZE));
		_snd.dma_slot_phys[i] = _snd.dma_data_base_phy + (i * DMA_BUF_SIZE);
	}
	audio_queue_reset();

	_snd.dma_cbs_phy = dma_phy_addr(0, _snd.dma_cbs_addr);
	for (uint32_t i = 0; i < DMA_BUFFER_SLOTS; i++) {
		_snd.dma_cbs[i].ti = DMA_DEST_DREQ + DMA_PERMAP_PWM + DMA_SRC_INC;
		_snd.dma_cbs[i].source_ad = (uint32_t)_snd.dma_slot_phys[i] | DMA_VC_ALIAS_UNCACHED;
		_snd.dma_cbs[i].dest_ad = PWM_FIFO_BUS_ADDR;
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
		slog("sound: unsupported bit depth: %d\n", cfg->bit_depth);
		return -1;
	}
	if (cfg->rate < 8000 || cfg->rate > 96000) {
		slog("sound: unsupported rate: %d\n", cfg->rate);
		return -1;
	}
	if (cfg->channels < 1 || cfg->channels > 2) {
		slog("sound: unsupported channels: %d\n", cfg->channels);
		return -1;
	}
	if (cfg->period_size <= 0 || cfg->period_count <= 0) {
		slog("sound: invalid period config: %d x %d\n",
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
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;

	if (!_snd.prepared) {
		return -1;
	}
	if (_snd.started) {
		return 0;
	}

	*(pwm + BCM283x_PWM_STATUS) = ERRORMASK;
	audio_set_pwm_range(_snd.pwm_range);
	*(pwm + BCM283x_PWM_CONTROL) = audio_pwm_control_flags();
	*(pwm + BCM283x_PWM_DMAC) = BCM283x_PWM_ENAB + 0x0707;
	_snd.started = true;
	return 0;
}

static int audio_stop(void) {
	if (_snd.started) {
		volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
		volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
		volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;
		uint32_t dma_enable_bits;
		*(dma + DMA_CS) = DMA_RESET;
		dma_enable_bits = *dmae;
		*dmae = dma_enable_bits & (~DMA_ENABLE_BIT);
		*(pwm + BCM283x_PWM_DMAC) = 0;
		*(pwm + BCM283x_PWM_CONTROL) = 0;
		audio_queue_reset();
		_snd.dma_started_usec = 0;
		_snd.dma_expected_usec = 0;
		_snd.dma_running = false;
		_snd.started = false;
	}
	return 0;
}

static int sound_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info, int oflag, void *p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(oflag);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	if (_snd.open_count > 0 && _snd.occupied_pid != from_pid) {
		return -1;
	}
	audio_stop();
	audio_deinit();
	_snd.occupied_pid = from_pid;
	_snd.open_count++;
	return 0;
}

static int sound_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t *info, void *p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(node);
	UNUSED(info);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	if (_snd.occupied_pid != from_pid || _snd.open_count <= 0) {
		return -1;
	}

	_snd.open_count--;
	if (_snd.open_count > 0) {
		return 0;
	}

	audio_stop();
	audio_deinit();
	_snd.occupied_pid = 0;
	return 0;
}

static int sound_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t *node,
					   const void *buf, int size, int offset, void *p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(node);
	UNUSED(p);

	const uint8_t *src;
	int total = 0;
	int consumed;
	uint32_t avail_bytes;
	uint32_t pushed_frames;
	uint32_t wait_start_usec = 0;

	from_pid = proc_getpid(from_pid);
	if (size <= 0 || _snd.occupied_pid != from_pid) {
		return -1;
	}
	if (offset < 0 || offset >= size) {
		return 0;
	}
	if (!_snd.configured && audio_ensure_default_config() != 0) {
		return -1;
	}

	if (!_snd.prepared) {
		if (audio_prepare() != 0) {
			return -1;
		}
	}
	if (!_snd.started) {
		if (audio_start() != 0) {
			return -1;
		}
	}

	size -= offset;
	if (_snd.frame_bytes == 0) {
		return -1;
	}

	size = (size / (int)_snd.frame_bytes) * (int)_snd.frame_bytes;
	if (size == 0) {
		return 0;
	}

	src = (const uint8_t *)buf + offset;
	while (total < size) {
		consumed = size - total;
		avail_bytes = audio_queue_avail_bytes();
		if (avail_bytes < (uint32_t)consumed) {
			consumed = (int)avail_bytes;
		}
		consumed = (consumed / (int)_snd.frame_bytes) * (int)_snd.frame_bytes;

		if (consumed > 0) {
			pushed_frames = audio_queue_push_pcm(src + total, consumed);
			if (pushed_frames > 0) {
				wait_start_usec = 0;
				total += (int)(pushed_frames * _snd.frame_bytes);
				audio_try_append_running_chain(audio_now_usec());
				sound_pump(dev);
				continue;
			}
		}

		sound_pump(dev);
		if (wait_start_usec == 0) {
			wait_start_usec = audio_now_usec();
		}
		else {
			uint32_t now_usec = audio_now_usec();
			if (audio_elapsed_usec(wait_start_usec, now_usec) >= 500000U) {
				if (audio_force_recover_stall(now_usec)) {
					sound_pump(dev);
				}
				audio_log_diag("write wait", now_usec, audio_dma_current_cb_bus());
				wait_start_usec = now_usec;
			}
		}
		proc_usleep(sound_poll_sleep_usec());
	}

	return total;
}

static void sound_refresh_status(vdevice_t* dev) {
	if (dev == NULL) {
		return;
	}
	if (sound_has_pending_work()) {
		sound_pump(dev);
	}
}

static uint32_t sound_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(p);

	sound_refresh_status(dev);
	if (_snd.configured && audio_queue_avail_frames() > 0) {
		return VFS_EVT_WR;
	}
	return 0;
}

static void sound_pump(vdevice_t* dev) {
	bool wake_writer = false;
	bool start_dma = false;
	bool rebuffer_start = false;
	uint32_t slot = DMA_SLOT_INVALID;
	uint32_t samples = 0;
	uint32_t now_usec = audio_now_usec();

	audio_service_locked(now_usec, &wake_writer, &start_dma, &rebuffer_start,
			&slot, &samples);

	if (start_dma) {
		audio_start_dma_transfer(slot, samples, rebuffer_start);
	}
	if (wake_writer) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
	}
}

static int sound_loop(vdevice_t* dev, void* p) {
	UNUSED(p);
	if (sound_has_pending_work()) {
		sound_pump(dev);
	}
	proc_usleep(sound_poll_sleep_usec());
	return 0;
}

static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t *in, proto_t *ret, void *p) {
	UNUSED(p);

	int result = 0;
	struct pcm_config cfg;

	if (_snd.occupied_pid != proc_getpid(from_pid)) {
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
			sound_refresh_status(dev);
			result = (int)MIN(MIN(_snd.buffer_bytes, _snd.write_chunk_bytes),
					audio_queue_avail_bytes());
		}
		break;
	default:
		result = -1;
		break;
	}

	PF->addi(ret, result);
	return 0;
}

static void audio_hw_init(void) {
	volatile uint32_t* clk = (uint32_t*)(uintptr_t)CLOCK_BASE;

	bcm283x_gpio_config(40, GPIO_ALTF0);
	bcm283x_gpio_config(41, GPIO_ALTF0);

	proc_usleep(2000);

	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | (1 << 5);
	proc_usleep(2000);

	*(clk + BCM283x_PWMCLK_DIV)  = PM_PASSWORD |
			(PWM_CLOCK_DIV_INT << 12) | PWM_CLOCK_DIV_FRAC;
	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | 16 | PWM_CLOCK_SOURCE_SELECT;
	proc_usleep(2000);
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/sound0";

	_mmio_base = mmio_map();
	bcm283x_gpio_init();
	audio_hw_init();

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "bcm283x-snd");
	dev.open = sound_open;
	dev.close = sound_close;
	dev.write = sound_write;
	dev.dev_cntl = sound_dev_cntl;
	dev.loop_step = sound_loop;
	dev.check_poll_events = sound_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
	return 0;
}
