#include &lt;arch/bcm283x/gpio.h&gt;
#include &lt;ewoksys/vdevice.h&gt;
#include &lt;ewoksys/syscall.h&gt;
#include &lt;ewoksys/mmio.h&gt;
#include &lt;ewoksys/klog.h&gt;
#include &lt;ewoksys/proc.h&gt;
#include &lt;ewoksys/proto.h&gt;
#include &lt;sysinfo.h&gt;
#include &lt;string.h&gt;
#include &lt;unistd.h&gt;
#include &lt;ewoksys/dma.h&gt;
#include &lt;stdbool.h&gt;

#define UNUSED(v) ((void)(v))
#define MIN(a, b) ((a) &lt; (b) ? (a) : (b))

/* PCM Control Commands - Compatible with virt/machine.virt */
#define CTRL_PCM_DEV_HW 0xF0
#define CTRL_PCM_DEV_HW_FREE 0xF1
#define CTRL_PCM_DEV_PRPARE 0xF2
#define CTRL_PCM_BUF_AVAIL 0xF3

/* PCM States - Standard PCM state machine */
enum pcm_state_t {
	PCM_STATE_UNKOWN = 0,
	PCM_STATE_OPEN = 1 &lt;&lt; 0,
	PCM_STATE_SETUP = 1 &lt;&lt; 1,
	PCM_STATE_PREPARE = 1 &lt;&lt; 2,
	PCM_STATE_RUNNING = 1 &lt;&lt; 3,
	PCM_STATE_XRUN = 1 &lt;&lt; 4,
	PCM_STATE_STOPED = 1 &lt;&lt; 5,
};

/* PCM Config Structure - Standard structure */
struct pcm_config {
	int bit_depth;
	int rate;
	int channels;
	int period_size;
	int period_count;
	int start_threshold;
	int stop_threshold;
};

/* Hardware Definitions for PWM Audio (BCM283x) */
#define USE_PWM1
#if defined(USE_PWM1)
#define PWM_BASE        (_mmio_base + 0x20C000 + 0x800)
#define CLOCK_BASE      (_mmio_base + 0x101000 + 0x800)
#else
#define PWM_BASE        (_mmio_base + 0x20C000)
#define CLOCK_BASE      (_mmio_base + 0x101000)
#endif

static sys_info_t _sysinfo;
#define DMA_V_BASE      _sysinfo.sys_dma.v_base
#define DMA_ENABLE      (DMA_V_BASE + 0xFF0)

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

/* DMA Definitions */
#define DMA_CS        0
#define DMA_CONBLK_AD 1
#define DMA_EN0       1 &lt;&lt; 0
#define DMA_ACTIVE    1
#define DMA_DEST_DREQ 0x40
#define DMA_SRC_INC   0x100

#ifdef USE_PWM1
#define DMA_PERMAP  0x10000
#else
#define DMA_PERMAP  0x0
#endif

/* DMA Control Block Structure */
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

/* Sound Device Runtime Structure */
typedef struct {
	/* PCM Config */
	struct pcm_config pcm_cfg;
	
	/* DMA Related */
	dma_cb_t* dma_cb;
	uint32_t dma_data_addr;
	uint32_t dma_cb_phy;
	uint32_t dma_data_addr_phy;
	
	/* Circular Buffer Management */
	char* dma_area;           /* CPU-accessible DMA buffer */
	int32_t buffer_size;      /* Total buffer size in bytes */
	int32_t period_size;      /* Period size in bytes */
	int32_t periods;          /* Number of periods */
	int32_t frame_size;       /* Bytes per frame */
	int32_t boundary;         /* Boundary for wrap-around */
	
	/* Pointers */
	volatile int32_t appl_ptr;    /* Application pointer */
	volatile int32_t hw_ptr;      /* Hardware pointer */
	
	/* State Management */
	enum pcm_state_t state;
	bool configured;
	bool prepared;
	bool started;
	int open_count;
	int occupied_pid;
} snd_dev_t;

static snd_dev_t _snd = {0};

/* Helper: Calculate bytes per frame */
static inline int calc_frame_size(struct pcm_config* cfg) {
	return (cfg-&gt;channels * cfg-&gt;bit_depth / 8);
}

/* Helper: Get state string */
static inline char* pcm_state_str(enum pcm_state_t state) {
	switch (state) {
	case PCM_STATE_OPEN:
		return "OPEN";
	case PCM_STATE_SETUP:
		return "SETUP";
	case PCM_STATE_PREPARE:
		return "PREPARE";
	case PCM_STATE_RUNNING:
		return "RUNNING";
	case PCM_STATE_XRUN:
		return "XRUN";
	case PCM_STATE_STOPED:
		return "STOPED";
	default:
		return "UNKNOWN";
	}
}

/* Helper: Calculate available space for playback */
static inline int32_t play_avail(snd_dev_t* snd) {
	int32_t avail = snd-&gt;hw_ptr + snd-&gt;buffer_size - snd-&gt;appl_ptr;
	if (avail &lt; 0) {
		avail += snd-&gt;boundary;
	} else if (avail &gt;= snd-&gt;boundary) {
		avail -= snd-&gt;boundary;
	}
	return avail;
}

/* Helper: Calculate frames ready for playback */
static inline int32_t frames_ready(snd_dev_t* snd) {
	int frames = snd-&gt;buffer_size - play_avail(snd);
	if (frames &gt;= 0) {
		return frames;
	}
	return 0;
}

/* Initialize PWM Hardware */
static void audio_hw_init(void) {
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&amp;_sysinfo);

	/* Configure GPIO for PWM */
#ifdef USE_PWM1
	bcm283x_gpio_config(40, GPIO_ALTF5);
	bcm283x_gpio_config(41, GPIO_ALTF5);
#else
	bcm283x_gpio_config(18, GPIO_ALTF5);
#endif

	proc_usleep(2000);

	volatile uint32_t* clk = (void*)CLOCK_BASE;
	
	/* Disable PWM Clock */
	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | (1 &lt;&lt; 5);
	proc_usleep(2000);

	/* Configure Divider (for 44.1kHz approx) */
	int idiv = 2;
	*(clk + BCM283x_PWMCLK_DIV)  = PM_PASSWORD | (idiv &lt;&lt; 12);
	proc_usleep(2000);

	/* Enable Clock */
	*(clk + BCM283x_PWMCLK_CNTL) = PM_PASSWORD | 16 | 1;
	proc_usleep(2000);
}

/* De-initialize Audio Hardware */
static void audio_deinit(void) {
	/* Stop PWM */
	volatile uint32_t *pwm = (uint32_t *)PWM_BASE;
	*(pwm + BCM283x_PWM_CONTROL) = 0;

	/* Stop DMA */
	volatile uint32_t *dma = (uint32_t *)DMA_V_BASE;
	*(dma + DMA_CS) = 0;

	/* Free DMA buffers */
	if (_snd.dma_cb != NULL) {
		dma_free(0, (ewokos_addr_t)_snd.dma_cb);
		_snd.dma_cb = NULL;
	}
	if (_snd.dma_data_addr != 0) {
		dma_free(0, _snd.dma_data_addr);
		_snd.dma_area = NULL;
		_snd.dma_data_addr = 0;
	}

	_snd.configured = false;
	_snd.prepared = false;
	_snd.started = false;
	_snd.state = PCM_STATE_UNKOWN;
}

/* Get PWM Range value based on sample rate */
static uint32_t get_pwm_range(int rate) {
	switch (rate) {
	case 8000:
		return 6750;
	case 16000:
		return 3375;
	case 44100:
		return 1228;
	case 48000:
		return 1125;
	default:
		return 1228; /* Default 44100 */
	}
}

/* Setup Hardware Parameters */
static int audio_hw_params(struct pcm_config *cfg) {
	volatile uint32_t *pwm = (uint32_t *)PWM_BASE;
	
	/* Validate configuration */
	if (cfg-&gt;bit_depth != 8 &amp;&amp; cfg-&gt;bit_depth != 16) {
		klog("sound: unsupported bit depth: %d\n", cfg-&gt;bit_depth);
		return -1;
	}
	if (cfg-&gt;rate != 8000 &amp;&amp; cfg-&gt;rate != 16000 &amp;&amp; cfg-&gt;rate != 44100 &amp;&amp; cfg-&gt;rate != 48000) {
		klog("sound: unsupported rate: %d\n", cfg-&gt;rate);
		return -1;
	}
	if (cfg-&gt;channels &lt; 1 || cfg-&gt;channels &gt; 2) {
		klog("sound: unsupported channels: %d\n", cfg-&gt;channels);
		return -1;
	}

	/* Free previous resources if any */
	audio_deinit();

	/* Save configuration */
	memcpy(&amp;_snd.pcm_cfg, cfg, sizeof(*cfg));
	
	/* Calculate sizes */
	_snd.frame_size = calc_frame_size(cfg);
	_snd.period_size = cfg-&gt;period_size * _snd.frame_size;
	_snd.periods = cfg-&gt;period_count;
	_snd.buffer_size = _snd.period_size * _snd.periods;
	_snd.boundary = _snd.buffer_size;
	
	/* Setup PWM Range */
	uint32_t range_val = get_pwm_range(cfg-&gt;rate);
	*(pwm + BCM283x_PWM_CONTROL) = 0;
	proc_usleep(2000);
	*(pwm + BCM283x_PWM_RANGE) = range_val;
	*(pwm + BCM283x_PWM_CONTROL) = BCM283x_PWM_USEFIFO | BCM283x_PWM_ENABLE | (1 &lt;&lt; 6);

	/* Allocate DMA Buffers */
	_snd.dma_cb = (dma_cb_t*)(dma_alloc(0, sizeof(dma_cb_t)));
	_snd.dma_data_addr = dma_alloc(0, _snd.buffer_size);
	_snd.dma_area = (char*)_snd.dma_data_addr;

	if (_snd.dma_cb == NULL || _snd.dma_data_addr == 0) {
		klog("sound: failed to allocate DMA buffer\n");
		audio_deinit();
		return -1;
	}

	/* Clear DMA area */
	memset(_snd.dma_area, 0, _snd.buffer_size);

	/* Get Physical Addresses */
	_snd.dma_cb_phy = dma_phy_addr(0, (ewokos_addr_t)_snd.dma_cb);
	_snd.dma_data_addr_phy = dma_phy_addr(0, _snd.dma_data_addr);

	/* Setup DMA Control Block for circular transfer */
	uint32_t phy_pwm_base = syscall1(SYS_V2P, PWM_BASE);
	_snd.dma_cb-&gt;ti = DMA_DEST_DREQ | DMA_PERMAP | DMA_SRC_INC;
	_snd.dma_cb-&gt;source_ad = _snd.dma_data_addr_phy;
	_snd.dma_cb-&gt;dest_ad = phy_pwm_base + 0x18; /* PWM FIFO address */
	_snd.dma_cb-&gt;txfr_len = _snd.buffer_size;
	_snd.dma_cb-&gt;stride = 0;
	_snd.dma_cb-&gt;nextconbk = _snd.dma_cb_phy; /* Loop to self */
	_snd.dma_cb-&gt;null1 = 0;
	_snd.dma_cb-&gt;null2 = 0;

	/* Reset pointers */
	_snd.appl_ptr = 0;
	_snd.hw_ptr = 0;

	_snd.configured = true;
	_snd.state = PCM_STATE_SETUP;
	
	klog("sound: hw_params done: rate=%d, bits=%d, ch=%d, buf=%d\n", 
	     cfg-&gt;rate, cfg-&gt;bit_depth, cfg-&gt;channels, _snd.buffer_size);
	return 0;
}

/* Free Hardware Resources */
static int audio_hw_free(void) {
	if (!_snd.configured) {
		return -1;
	}
	
	audio_deinit();
	return 0;
}

/* Prepare Audio for Playback */
static int audio_prepare(void) {
	switch (_snd.state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_STOPED:
	case PCM_STATE_XRUN:
		break;
	case PCM_STATE_OPEN:
		return -1;
	case PCM_STATE_RUNNING:
		return -1;
	default:
		return -1;
	}

	if (!_snd.configured) {
		return -1;
	}

	/* Reset pointers */
	_snd.appl_ptr = _snd.hw_ptr = 0;
	_snd.prepared = true;
	_snd.state = PCM_STATE_PREPARE;
	
	return 0;
}

/* Start Audio Playback */
static int audio_start(void) {
	if (_snd.state != PCM_STATE_PREPARE &amp;&amp; _snd.state != PCM_STATE_STOPED) {
		return -1;
	}

	/* Enable PWM DMA */
	volatile uint32_t *pwm = (uint32_t *)PWM_BASE;
	volatile uint32_t *dma = (uint32_t *)DMA_V_BASE;
	volatile uint32_t *dmae = (uint32_t *)DMA_ENABLE;

	*(pwm + BCM283x_PWM_DMAC) = BCM283x_PWM_ENAB | 0x0707;
	*dmae = DMA_EN0;
	
	/* Start DMA */
	*(dma + DMA_CONBLK_AD) = _snd.dma_cb_phy;
	proc_usleep(200);
	*(dma + DMA_CS) = DMA_ACTIVE;

	_snd.started = true;
	_snd.state = PCM_STATE_RUNNING;
	
	return 0;
}

/* Stop Audio Playback */
static int audio_stop(void) {
	if (_snd.started) {
		volatile uint32_t *pwm = (uint32_t *)PWM_BASE;
		*(pwm + BCM283x_PWM_CONTROL) = 0;
		
		volatile uint32_t *dma = (uint32_t *)DMA_V_BASE;
		*(dma + DMA_CS) = 0;
		
		_snd.started = false;
	}

	if (_snd.state == PCM_STATE_RUNNING) {
		_snd.state = PCM_STATE_STOPED;
	}
	
	return 0;
}

/* Copy data to circular buffer */
static int do_transfer(const void* src, int src_off, int appl_off, int frames) {
	char* hw_ptr = _snd.dma_area + (appl_off % _snd.buffer_size) * _snd.frame_size;
	char* user_ptr = (char*)src + src_off * _snd.frame_size;
	int to_end = _snd.buffer_size - (appl_off % _snd.buffer_size) * _snd.frame_size;
	int copy_bytes = frames * _snd.frame_size;
	
	if (copy_bytes &lt;= to_end) {
		memcpy(hw_ptr, user_ptr, copy_bytes);
	} else {
		/* Split copy for wrap-around */
		memcpy(hw_ptr, user_ptr, to_end);
		memcpy(_snd.dma_area, user_ptr + to_end, copy_bytes - to_end);
	}
	
	return 0;
}

/* Device Open */
static int sound_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, int oflag, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(oflag);
	UNUSED(p);

	if (_snd.open_count != 0) {
		klog("sound: device is busy\n");
		return -1;
	}

	_snd.open_count = 1;
	_snd.occupied_pid = proc_getpid(from_pid);
	_snd.state = PCM_STATE_OPEN;
	
	return 0;
}

/* Device Close */
static int sound_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* info, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(node);
	UNUSED(info);
	UNUSED(p);

	if (_snd.occupied_pid != proc_getpid(from_pid)) {
		return -1;
	}

	/* Stop and clean up */
	audio_stop();
	audio_hw_free();
	
	_snd.occupied_pid = 0;
	_snd.open_count = 0;
	_snd.state = PCM_STATE_UNKOWN;
	
	return 0;
}

/* Device Write */
static int sound_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
					   const void* buf, int size, int offset, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(info);
	UNUSED(p);
	UNUSED(offset);

	/* Check state and permissions */
	if (_snd.occupied_pid != proc_getpid(from_pid)) {
		return -1;
	}
	
	switch (_snd.state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		break;
	case PCM_STATE_XRUN:
		return -32; /* EPIPE */
	default:
		return -9; /* EBADF */
	}

	/* Calculate frames to write */
	int frames_to_write = size / _snd.frame_size;
	if (frames_to_write == 0 || offset != 0) {
		return 0;
	}

	int offset_frames = 0;
	int written = 0;
	int avail = 0;

	/* Main write loop */
	while (frames_to_write &gt; 0) {
		int copy_frames = 0;
		int to_end = 0;
		int appl_ptr = 0;
		int app_offset = 0;

		avail = play_avail(&amp;_snd);
		if (avail == 0) {
			/* No space available */
			if (written &gt; 0) {
				return written * _snd.frame_size;
			}
			return -11; /* EAGAIN */
		}

		copy_frames = frames_to_write &gt; avail ? avail : frames_to_write;
		
		/* Transfer data */
		app_offset = _snd.appl_ptr % _snd.boundary;
		do_transfer(buf, offset_frames, app_offset, copy_frames);
		
		/* Update pointers */
		appl_ptr = _snd.appl_ptr + copy_frames;
		if (appl_ptr &gt;= _snd.boundary) {
			appl_ptr -= _snd.boundary;
		}
		
		_snd.appl_ptr = appl_ptr;
		offset_frames += copy_frames;
		frames_to_write -= copy_frames;
		written += copy_frames;

		/* Auto-start if needed */
		if (_snd.state == PCM_STATE_PREPARE &amp;&amp; frames_ready(&amp;_snd) &gt;= _snd.pcm_cfg.start_threshold) {
			if (audio_start() == 0) {
				_snd.state = PCM_STATE_RUNNING;
			}
		}
	}

	return written * _snd.frame_size;
}

/* Device Control */
static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	UNUSED(dev);
	UNUSED(p);

	int result = 0;

	if (_snd.occupied_pid != proc_getpid(from_pid)) {
		return -1;
	}

	switch (cmd) {
	case CTRL_PCM_DEV_HW: {
		struct pcm_config config;
		memset(&amp;config, 0, sizeof(config));
		proto_read_to(in, &amp;config, sizeof(config));
		result = audio_hw_params(&amp;config);
		break;
	}
	case CTRL_PCM_DEV_HW_FREE:
		result = audio_hw_free();
		break;
	case CTRL_PCM_DEV_PRPARE:
		result = audio_prepare();
		break;
	case CTRL_PCM_BUF_AVAIL: {
		/* Calculate available buffer space */
		switch (_snd.state) {
		case PCM_STATE_PREPARE:
		case PCM_STATE_RUNNING:
		case PCM_STATE_SETUP:
			result = play_avail(&amp;_snd) * _snd.frame_size;
			break;
		case PCM_STATE_XRUN:
			result = -32; /* EPIPE */
			break;
		default:
			result = -9; /* EBADF */
			break;
		}
		break;
	}
	default:
		klog("sound: unknown ctrl cmd: %d\n", cmd);
		result = -22; /* EINVAL */
		break;
	}

	PF-&gt;addi(ret, result);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc &gt; 1 ? argv[1] : "/dev/sound";

	_mmio_base = mmio_map();
	bcm283x_gpio_init();
	audio_hw_init();

	vdevice_t dev;
	memset(&amp;dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "bcm283x-sound");
	dev.open = sound_open;
	dev.close = sound_close;
	dev.write = sound_write;
	dev.dev_cntl = sound_dev_cntl;

	klog("sound: driver initialized at %s\n", mnt_point);
	device_run(&amp;dev, mnt_point, FS_TYPE_CHAR, 0666);
	return 0;
}
