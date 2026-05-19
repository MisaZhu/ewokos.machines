#include <arch/bcm283x/gpio.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
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

#define DMA_BUF_SIZE  (1024*16)
#define DMA_SAMPLE_CAPACITY (DMA_BUF_SIZE / sizeof(uint32_t))
#define PWM_OUTPUT_CHANNELS 2U
#define PWM_BASE_CLOCK_HZ 100000000U
#define PWM_CLOCK_SOURCE_SELECT 6U
#define PWM_CLOCK_DIV_INT 5U
#define PWM_CLOCK_DIV_FRAC 0U
#define DMA_WAIT_SLICE_US 200
#define DMA_WAIT_RETRY_MAX 20000
#define SAMPLE_QUEUE_CAPACITY (DMA_SAMPLE_CAPACITY * 8U)
#define SOUND_LOOP_SLEEP_US 1000
#define DMA_WATCHDOG_MARGIN_US 200000U
#define SOUND_DEFAULT_BIT_DEPTH 16
#define SOUND_DEFAULT_RATE 48000
#define SOUND_DEFAULT_CHANNELS 2
#define SOUND_DEFAULT_PERIOD_SIZE 1024
#define SOUND_DEFAULT_PERIOD_COUNT 4
#define SOUND_BUILD_TAG "20260519qpush"

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
	dma_cb_t* dma_cb;
	ewokos_addr_t dma_cb_addr;
	ewokos_addr_t dma_data_addr;
	ewokos_addr_t dma_cb_phy;
	ewokos_addr_t dma_data_addr_phy;
	struct pcm_config pcm_cfg;
	uint32_t pwm_range;
	uint32_t frame_bytes;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
	uint32_t write_chunk_bytes;
	uint32_t* sample_queue;
	uint32_t queue_capacity;
	uint32_t queue_head;
	uint32_t queue_count;
	uint32_t dma_started_usec;
	uint32_t dma_expected_usec;
	bool configured;
	bool prepared;
	bool started;
	bool dma_running;
	int open_count;
	int occupied_pid;
} snd_dev_t;

static snd_dev_t _snd = {0};
static bool _log_first_open = true;
static bool _log_first_write = true;
static bool _log_first_dma = true;
static bool _log_first_pump = true;

static void sound_pump(vdevice_t* dev);

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
	_snd.queue_head = 0;
	_snd.queue_count = 0;
}

static uint32_t audio_queue_avail_frames(void) {
	if (_snd.queue_capacity < _snd.queue_count) {
		return 0;
	}
	return (_snd.queue_capacity - _snd.queue_count) / audio_output_words_per_frame();
}

static uint32_t audio_queue_avail_bytes(void) {
	return audio_queue_avail_frames() * _snd.frame_bytes;
}

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
			(uint64_t)(_snd.pwm_range - 1U)) >> 32;
	if (scaled >= _snd.pwm_range) {
		scaled = _snd.pwm_range - 1U;
	}
	return (uint32_t)scaled;
}

static void audio_frame_to_pwm_words(const uint8_t* frame, uint32_t* words) {
	uint32_t sample_bytes = audio_sample_bytes(_snd.pcm_cfg.bit_depth);
	int32_t left;
	int32_t right;

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

static uint32_t audio_queue_push_pcm(const uint8_t* buf, int size) {
	uint32_t frames = (uint32_t)(size / (int)_snd.frame_bytes);
	uint32_t avail = audio_queue_avail_frames();

	if (_snd.sample_queue == NULL || frames > avail) {
		return 0;
	}

	for (uint32_t i = 0; i < frames; i++) {
		uint32_t tail;
		uint32_t words[PWM_OUTPUT_CHANNELS];

		audio_frame_to_pwm_words(buf + (i * _snd.frame_bytes), words);
		for (uint32_t ch = 0; ch < audio_output_words_per_frame(); ch++) {
			tail = (_snd.queue_head + _snd.queue_count) % _snd.queue_capacity;
			_snd.sample_queue[tail] = words[ch];
			_snd.queue_count++;
		}
	}
	return frames;
}

static void audio_set_pwm_range(uint32_t range) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;

	*(pwm + BCM283x_PWM_RANGE1) = range;
	*(pwm + BCM283x_PWM_RANGE2) = range;
}

static uint32_t audio_queue_fill_dma_buffer(void) {
	uint32_t frames = _snd.queue_count;
	uint32_t* pwm_data = (uint32_t*)(uintptr_t)_snd.dma_data_addr;

	if (frames > DMA_SAMPLE_CAPACITY) {
		frames = DMA_SAMPLE_CAPACITY;
	}

	for (uint32_t i = 0; i < frames; i++) {
		pwm_data[i] = _snd.sample_queue[(_snd.queue_head + i) % _snd.queue_capacity];
	}
	return frames;
}

static void audio_queue_drop_frames(uint32_t frames) {
	if (frames > _snd.queue_count || _snd.queue_capacity == 0) {
		audio_queue_reset();
		return;
	}
	_snd.queue_head = (_snd.queue_head + frames) % _snd.queue_capacity;
	_snd.queue_count -= frames;
}

static bool audio_dma_active(void) {
	volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
	return ((*(dma + DMA_CS)) & DMA_ACTIVE) != 0;
}

static int audio_start_dma_transfer(uint32_t samples) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
	volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
	volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;
	uint32_t dma_enable_bits;
	uint32_t cb_bus;
	uint32_t src_bus;

	if (samples == 0) {
		return 0;
	}

	cb_bus = (uint32_t)_snd.dma_cb_phy | DMA_VC_ALIAS_UNCACHED;
	src_bus = (uint32_t)_snd.dma_data_addr_phy | DMA_VC_ALIAS_UNCACHED;
	_snd.dma_cb->source_ad = src_bus;

	_snd.dma_cb->txfr_len = samples * sizeof(uint32_t);
	*(pwm + BCM283x_PWM_STATUS) = ERRORMASK;
	audio_set_pwm_range(_snd.pwm_range);
	*(pwm + BCM283x_PWM_CONTROL) = audio_pwm_control_flags();
	*(pwm + BCM283x_PWM_DMAC) = BCM283x_PWM_ENAB + 0x0707;
	*(dma + DMA_CS) = DMA_RESET;
	proc_usleep(DMA_WAIT_SLICE_US);
	dma_enable_bits = *dmae;
	*dmae = dma_enable_bits | DMA_ENABLE_BIT;
	*(dma + DMA_CONBLK_AD) = cb_bus;
	proc_usleep(DMA_WAIT_SLICE_US);
	*(dma + DMA_CS) = DMA_ACTIVE | DMA_PRIORITY_DEFAULT | DMA_PANIC_PRIORITY_DEFAULT;
	_snd.dma_started_usec = audio_now_usec();
	_snd.dma_expected_usec = audio_dma_watchdog_usec(samples);
	_snd.dma_running = true;
	return 0;
}

static void audio_service_locked(uint32_t now_usec, bool* wake_writer,
		bool* start_dma, uint32_t* samples) {
	if (!_snd.started) {
		return;
	}

	if (_snd.dma_running && !audio_dma_active()) {
		_snd.dma_running = false;
		_snd.dma_started_usec = 0;
		_snd.dma_expected_usec = 0;
		*wake_writer = true;
	}
	else if (_snd.dma_running &&
			_snd.dma_started_usec != 0 &&
			audio_elapsed_usec(_snd.dma_started_usec, now_usec) > _snd.dma_expected_usec) {
		_snd.dma_running = false;
		_snd.dma_started_usec = 0;
		_snd.dma_expected_usec = 0;
		*wake_writer = true;
	}

	if (!_snd.dma_running && _snd.queue_count > 0 && *samples == 0) {
		*samples = audio_queue_fill_dma_buffer();
		if (*samples > 0) {
			audio_queue_drop_frames(*samples);
			*start_dma = true;
			*wake_writer = true;
		}
	}
}

static void audio_deinit(void) {
	if (_snd.dma_cb_addr != 0) {
		dma_free(0, _snd.dma_cb_addr);
		_snd.dma_cb_addr = 0;
		_snd.dma_cb = NULL;
	}
	if (_snd.dma_data_addr != 0) {
		dma_free(0, _snd.dma_data_addr);
		_snd.dma_data_addr = 0;
	}
	if (_snd.sample_queue != NULL) {
		free(_snd.sample_queue);
		_snd.sample_queue = NULL;
	}
	_snd.dma_cb_phy = 0;
	_snd.dma_data_addr_phy = 0;
	_snd.pwm_range = 0;
	_snd.frame_bytes = 0;
	_snd.period_bytes = 0;
	_snd.buffer_bytes = 0;
	_snd.write_chunk_bytes = 0;
	_snd.queue_capacity = 0;
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
	*(pwm + BCM283x_PWM_CONTROL) = 0;
	proc_usleep(2000);
	_snd.pwm_range = audio_rate_to_pwm_range(cfg->rate);
	audio_set_pwm_range(_snd.pwm_range);
	*(pwm + BCM283x_PWM_CONTROL) = audio_pwm_control_flags();

	_snd.dma_cb_addr = dma_alloc(0, sizeof(dma_cb_t));
	_snd.dma_cb = (dma_cb_t*)(uintptr_t)_snd.dma_cb_addr;
	_snd.dma_data_addr = (ewokos_addr_t)dma_alloc(0, DMA_BUF_SIZE);

	if (_snd.dma_cb_addr == 0 || _snd.dma_cb == NULL || _snd.dma_data_addr == 0) {
		audio_deinit();
		return -1;
	}

	_snd.sample_queue = (uint32_t*)malloc(sizeof(uint32_t) * SAMPLE_QUEUE_CAPACITY);
	if (_snd.sample_queue == NULL) {
		audio_deinit();
		return -1;
	}
	_snd.queue_capacity = SAMPLE_QUEUE_CAPACITY;
	audio_queue_reset();

	_snd.dma_cb_phy = dma_phy_addr(0, _snd.dma_cb_addr);
	_snd.dma_data_addr_phy = dma_phy_addr(0, _snd.dma_data_addr);
	_snd.dma_cb->ti = DMA_DEST_DREQ + DMA_PERMAP_PWM + DMA_SRC_INC;
	_snd.dma_cb->source_ad = (uint32_t)_snd.dma_data_addr_phy | DMA_VC_ALIAS_UNCACHED;
	_snd.dma_cb->dest_ad = PWM_FIFO_BUS_ADDR;
	_snd.dma_cb->stride = 0x00;
	_snd.dma_cb->nextconbk = 0x00;
	_snd.dma_cb->null1 = 0x00;
	_snd.dma_cb->null2 = 0x00;

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

	audio_deinit();

	memcpy(&_snd.pcm_cfg, cfg, sizeof(*cfg));
	_snd.frame_bytes = (uint32_t)cfg->channels * sample_bytes;
	_snd.period_bytes = (uint32_t)cfg->period_size * _snd.frame_bytes;
	_snd.buffer_bytes = _snd.period_bytes * (uint32_t)cfg->period_count;
	_snd.write_chunk_bytes = _snd.frame_bytes *
			(DMA_SAMPLE_CAPACITY / audio_output_words_per_frame());
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
		slog("sound: busy occupied_pid=%d req_pid=%d\n", _snd.occupied_pid, from_pid);
		return -1;
	}
	audio_stop();
	audio_deinit();
	_snd.occupied_pid = from_pid;
	_snd.open_count++;
	_log_first_write = true;
	_log_first_dma = true;
	_log_first_pump = true;
	if (_log_first_open) {
		_log_first_open = false;
	}
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

	from_pid = proc_getpid(from_pid);
	if (size <= 0 || _snd.occupied_pid != from_pid) {
		slog("sound: write rejected size=%d occupied_pid=%d req_pid=%d configured=%d\n",
				size, _snd.occupied_pid, from_pid, _snd.configured ? 1 : 0);
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
	if (_log_first_write) {
		_log_first_write = false;
	}
	while (total < size) {
		consumed = size - total;
		avail_bytes = audio_queue_avail_bytes();
		if (avail_bytes < (uint32_t)consumed) {
			consumed = (int)avail_bytes;
		}
		consumed = (consumed / (int)_snd.frame_bytes) * (int)_snd.frame_bytes;

		if (consumed > 0) {
			pushed_frames = audio_queue_push_pcm(src + total, consumed);
			avail_bytes = _snd.queue_count;
			if (pushed_frames > 0) {
				total += consumed;
				sound_pump(dev);
				continue;
			}
		}

		sound_pump(dev);
		proc_usleep(SOUND_LOOP_SLEEP_US);
	}

	return total;
}

static uint32_t sound_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(p);

	if (_snd.configured && audio_queue_avail_frames() > 0) {
		return VFS_EVT_WR;
	}
	return 0;
}

static void sound_pump(vdevice_t* dev) {
	bool wake_writer = false;
	bool start_dma = false;
	uint32_t samples = 0;
	uint32_t now_usec = audio_now_usec();

	audio_service_locked(now_usec, &wake_writer, &start_dma, &samples);
	if (_log_first_pump && (_snd.started || _snd.queue_count > 0)) {
		_log_first_pump = false;
	}

	if (start_dma) {
		if (audio_start_dma_transfer(samples) != 0) {
			slog("sound: failed to start dma transfer\n");
		}
	}
	if (wake_writer) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
	}
}

static int sound_loop(vdevice_t* dev, void* p) {
	UNUSED(p);
	sound_pump(dev);
	proc_usleep(SOUND_LOOP_SLEEP_US);
	return 0;
}

static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t *in, proto_t *ret, void *p) {
	UNUSED(dev);
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
