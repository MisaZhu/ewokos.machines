#include <arch/bcm283x/gpio.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <string.h>
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

#define USE_PWM1

#if defined(USE_PWM1)
#define PWM_BASE        (_mmio_base + 0x20C000 + 0x800)
#define CLOCK_BASE      (_mmio_base + 0x101000 + 0x800)
#else
#define PWM_BASE        (_mmio_base + 0x20C000)
#define CLOCK_BASE      (_mmio_base + 0x101000)
#endif

#define DMA_BASE        (_mmio_base + 0x007000)
#define DMA_ENABLE      (_mmio_base + 0x007FF0)

#define BCM283x_PWMCLK_CNTL 40
#define BCM283x_PWMCLK_DIV  41
#define PM_PASSWORD 0x5A000000

#define BCM283x_PWM_CONTROL 0
#define BCM283x_PWM_STATUS  1
#define BCM283x_PWM_DMAC    2
#define BCM283x_PWM_FIFO    6

#ifdef USE_PWM1
#define BCM283x_PWM_RANGE  8
#define BCM283x_PWM_DATA   9
#else
#define BCM283x_PWM_RANGE  4
#define BCM283x_PWM_DATA   5
#endif

#ifdef USE_PWM1
#define BCM283x_PWM_USEFIFO  0x2000
#define BCM283x_PWM_ENABLE   0x0100
#else
#define BCM283x_PWM_USEFIFO  0x0020
#define BCM283x_PWM_ENABLE   0x0001
#endif

#define BCM283x_PWM_ENAB      0x80000000

#define BCM283x_GAPO2 0x20
#define BCM283x_GAPO1 0x10
#define BCM283x_RERR1 0x8
#define BCM283x_WERR1 0x4
#define BCM283x_FULL1 0x1
#define ERRORMASK (BCM283x_GAPO2 | BCM283x_GAPO1 | BCM283x_RERR1 | BCM283x_WERR1)

#define DMA_CS        0
#define DMA_CONBLK_AD 1
#define DMA_EN0       1 << 0
#define DMA_ACTIVE    1
#define DMA_DEST_DREQ 0x40
#define DMA_SRC_INC   0x100

#ifdef USE_PWM1
#define DMA_PERMAP  0x10000
#else
#define DMA_PERMAP  0x0
#endif

#define DMA_BUF_SIZE  (1024*16)
#define DMA_SAMPLE_CAPACITY (DMA_BUF_SIZE / sizeof(uint32_t))
#define PWM_BASE_CLOCK_HZ 54000000U
#define DMA_WAIT_SLICE_US 200
#define DMA_WAIT_RETRY_MAX 20000

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
	ewokos_addr_t dma_data_addr;
	ewokos_addr_t dma_cb_phy;
	ewokos_addr_t dma_data_addr_phy;
	struct pcm_config pcm_cfg;
	uint32_t pwm_range;
	uint32_t frame_bytes;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
	uint32_t write_chunk_bytes;
	bool configured;
	bool prepared;
	bool started;
	int open_count;
	int occupied_pid;
} snd_dev_t;

static snd_dev_t _snd = {0};

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

static uint32_t audio_rate_to_pwm_range(int rate) {
	uint64_t range = ((uint64_t)PWM_BASE_CLOCK_HZ + ((uint64_t)rate / 2ULL)) / (uint64_t)rate;
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

static uint32_t audio_frame_to_pwm(const uint8_t* frame) {
	uint32_t sample_bytes = audio_sample_bytes(_snd.pcm_cfg.bit_depth);
	int64_t mixed = 0;
	uint64_t scaled;
	int32_t averaged;

	for (int ch = 0; ch < _snd.pcm_cfg.channels; ch++) {
		mixed += audio_pcm_sample_to_s32(frame + ((uint32_t)ch * sample_bytes), sample_bytes);
	}

	averaged = (int32_t)(mixed / _snd.pcm_cfg.channels);
	scaled = (((uint64_t)((int64_t)averaged + 2147483648LL)) *
			(uint64_t)(_snd.pwm_range - 1U)) >> 32;
	if (scaled >= _snd.pwm_range) {
		scaled = _snd.pwm_range - 1U;
	}
	return (uint32_t)scaled;
}

static uint32_t audio_fill_dma_buffer(const uint8_t* buf, int size) {
	uint32_t frames = (uint32_t)(size / (int)_snd.frame_bytes);
	uint32_t* pwm_data = (uint32_t*)(uintptr_t)_snd.dma_data_addr;

	if (frames > DMA_SAMPLE_CAPACITY) {
		frames = DMA_SAMPLE_CAPACITY;
	}

	for (uint32_t i = 0; i < frames; i++) {
		pwm_data[i] = audio_frame_to_pwm(buf + (i * _snd.frame_bytes));
	}
	return frames;
}

static int audio_run_dma_transfer(uint32_t samples) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
	volatile uint32_t *dma = (uint32_t *)(uintptr_t)DMA_BASE;
	volatile uint32_t *dmae = (uint32_t *)(uintptr_t)DMA_ENABLE;
	int retry = 0;

	if (samples == 0) {
		return 0;
	}

	_snd.dma_cb->txfr_len = samples * sizeof(uint32_t);

	*(pwm + BCM283x_PWM_STATUS) = ERRORMASK;
	*(pwm + BCM283x_PWM_RANGE) = _snd.pwm_range;
	*(pwm + BCM283x_PWM_CONTROL) = BCM283x_PWM_USEFIFO | BCM283x_PWM_ENABLE | (1 << 6);
	*(pwm + BCM283x_PWM_DMAC) = BCM283x_PWM_ENAB + 0x0707;

	*dmae = DMA_EN0;
	*(dma + DMA_CONBLK_AD) = (uint32_t)_snd.dma_cb_phy;
	proc_usleep(DMA_WAIT_SLICE_US);
	*(dma + DMA_CS) = DMA_ACTIVE;

	while ((*(dma + DMA_CS)) & DMA_ACTIVE) {
		if (retry++ > DMA_WAIT_RETRY_MAX) {
			klog("sound: dma transfer timed out\n");
			return -1;
		}
		proc_usleep(DMA_WAIT_SLICE_US);
	}

	return 0;
}

static void audio_deinit(void) {
	if (_snd.dma_cb != NULL) {
		dma_free(0, (ewokos_addr_t)_snd.dma_cb);
		_snd.dma_cb = NULL;
	}
	if (_snd.dma_data_addr != 0) {
		dma_free(0, _snd.dma_data_addr);
		_snd.dma_data_addr = 0;
	}
	_snd.dma_cb_phy = 0;
	_snd.dma_data_addr_phy = 0;
	_snd.pwm_range = 0;
	_snd.frame_bytes = 0;
	_snd.period_bytes = 0;
	_snd.buffer_bytes = 0;
	_snd.write_chunk_bytes = 0;
	memset(&_snd.pcm_cfg, 0, sizeof(_snd.pcm_cfg));
	_snd.configured = false;
	_snd.prepared = false;
	_snd.started = false;
}

static int audio_init_pcm(const struct pcm_config *cfg) {
	volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
	uint32_t phy_pwm_base;

	*(pwm + BCM283x_PWM_CONTROL) = 0;
	proc_usleep(2000);
	_snd.pwm_range = audio_rate_to_pwm_range(cfg->rate);
	*(pwm + BCM283x_PWM_RANGE) = _snd.pwm_range;
	*(pwm + BCM283x_PWM_CONTROL) = BCM283x_PWM_USEFIFO | BCM283x_PWM_ENABLE | (1 << 6);

	_snd.dma_cb = (dma_cb_t*)(dma_alloc(0, sizeof(dma_cb_t)));
	_snd.dma_data_addr = (ewokos_addr_t)dma_alloc(0, DMA_BUF_SIZE);

	if (_snd.dma_cb == NULL || _snd.dma_data_addr == 0) {
		klog("sound: failed to allocate DMA buffer\n");
		audio_deinit();
		return -1;
	}

	_snd.dma_cb_phy = dma_phy_addr(0, (ewokos_addr_t)_snd.dma_cb);
	_snd.dma_data_addr_phy = dma_phy_addr(0, _snd.dma_data_addr);

	phy_pwm_base = (uint32_t)syscall1(SYS_V2P, (ewokos_addr_t)PWM_BASE);
	_snd.dma_cb->ti = DMA_DEST_DREQ + DMA_PERMAP + DMA_SRC_INC;
	_snd.dma_cb->source_ad = (uint32_t)_snd.dma_data_addr_phy;
	_snd.dma_cb->dest_ad = phy_pwm_base + 0x18;
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
		klog("sound: unsupported bit depth: %d\n", cfg->bit_depth);
		return -1;
	}
	if (cfg->rate < 8000 || cfg->rate > 96000) {
		klog("sound: unsupported rate: %d\n", cfg->rate);
		return -1;
	}
	if (cfg->channels < 1 || cfg->channels > 2) {
		klog("sound: unsupported channels: %d\n", cfg->channels);
		return -1;
	}
	if (cfg->period_size <= 0 || cfg->period_count <= 0) {
		klog("sound: invalid period config: %d x %d\n",
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
	_snd.write_chunk_bytes = _snd.frame_bytes * DMA_SAMPLE_CAPACITY;
	return audio_init_pcm(cfg);
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
	_snd.started = true;
	return 0;
}

static int audio_stop(void) {
	if (_snd.started) {
		volatile uint32_t *pwm = (uint32_t *)(uintptr_t)PWM_BASE;
		*(pwm + BCM283x_PWM_CONTROL) = 0;
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

	int total = 0;
	int consumed;
	uint32_t samples;

	from_pid = proc_getpid(from_pid);
	if (!_snd.configured || size <= 0 || _snd.occupied_pid != from_pid) {
		return -1;
	}
	if (offset < 0 || offset >= size) {
		return 0;
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

	while (total < size) {
		consumed = MIN(size - total, (int)_snd.write_chunk_bytes);
		samples = audio_fill_dma_buffer((const uint8_t *)buf + offset + total, consumed);
		if (samples == 0) {
			break;
		}
		if (audio_run_dma_transfer(samples) != 0) {
			if (total == 0) {
				return -1;
			}
			break;
		}
		total += (int)(samples * _snd.frame_bytes);
	}

	return total;
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
		if (!_snd.configured) {
			result = -1;
		}
		else if (_snd.buffer_bytes == 0 || _snd.write_chunk_bytes == 0) {
			result = -1;
		}
		else {
			result = (int)MIN(_snd.buffer_bytes, _snd.write_chunk_bytes);
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

#ifdef USE_PWM1
	bcm283x_gpio_config(40, GPIO_ALTF5);
	bcm283x_gpio_config(41, GPIO_ALTF5);
#else
	bcm283x_gpio_config(18, GPIO_ALTF5);
#endif

	proc_usleep(2000);

	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | (1 << 5);
	proc_usleep(2000);

	int idiv = 2;
	*(clk + BCM283x_PWMCLK_DIV)  = PM_PASSWORD | (idiv<<12);
	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | 16 | 1;
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

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
	return 0;
}
