#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>

#include "dma_chain.h"
#include "i2s.h"
#include "wm8960.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define UNUSED(v) ((void)(v))

#define CTRL_PCM_DEV_HW 0xF0
#define CTRL_PCM_DEV_HW_FREE 0xF1
#define CTRL_PCM_DEV_PRPARE 0xF2
#define CTRL_PCM_BUF_AVAIL 0xF3

#define SOUND_DEFAULT_BIT_DEPTH 16
#define SOUND_DEFAULT_RATE 48000
#define SOUND_DEFAULT_CHANNELS 2
#define SOUND_DEFAULT_PERIOD_SIZE 1024
#define SOUND_DEFAULT_PERIOD_COUNT 4
#define SOUND_WRITE_SLEEP_US 100
#define SOUND_HW_BIT_DEPTH 16
#define SOUND_HW_RATE 48000
#define SOUND_HW_CHANNELS 2
#define SOUND_HW_SAMPLE_BYTES 2
#define SOUND_HW_FRAME_BYTES (SOUND_HW_CHANNELS * SOUND_HW_SAMPLE_BYTES)
#define SOUND_CONVERT_BUF_FRAMES 1024
#define SOUND_CONVERT_BUF_BYTES (SOUND_CONVERT_BUF_FRAMES * SOUND_HW_FRAME_BYTES)

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
	uint32_t sample_bytes;
	uint32_t frame_bytes;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
	uint32_t write_chunk_bytes;
	uint32_t resample_accum;
	int16_t hold_left;
	int16_t hold_right;
	bool hold_valid;
	bool configured;
	bool prepared;
	bool started;
	int open_count;
	int occupied_pid;
} sound_state_t;

static sound_state_t _sound = {0};
static pthread_mutex_t _sound_lock;

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

static void audio_reset_convert_state_locked(void) {
	_sound.resample_accum = 0;
	_sound.hold_left = 0;
	_sound.hold_right = 0;
	_sound.hold_valid = false;
}

static void audio_decode_frame_to_s16_stereo_locked(const uint8_t* frame, int16_t* left, int16_t* right) {
	int32_t left32;
	int32_t right32;

	left32 = audio_pcm_sample_to_s32(frame, _sound.sample_bytes);
	if (_sound.pcm_cfg.channels > 1) {
		right32 = audio_pcm_sample_to_s32(frame + _sound.sample_bytes, _sound.sample_bytes);
	}
	else {
		right32 = left32;
	}

	*left = audio_clip_s16(left32 >> 16);
	*right = audio_clip_s16(right32 >> 16);
}

static void audio_store_hw_frame(uint8_t* dst, int16_t left, int16_t right) {
	dst[0] = (uint8_t)(left & 0xFF);
	dst[1] = (uint8_t)(((uint16_t)left >> 8) & 0xFF);
	dst[2] = (uint8_t)(right & 0xFF);
	dst[3] = (uint8_t)(((uint16_t)right >> 8) & 0xFF);
}

static uint32_t audio_pending_output_frames_locked(void) {
	if (!_sound.hold_valid || _sound.pcm_cfg.rate <= 0) {
		return 0;
	}
	return _sound.resample_accum / (uint32_t)_sound.pcm_cfg.rate;
}

static uint32_t audio_input_avail_bytes_locked(void) {
	uint32_t out_avail_frames;
	uint32_t pending_frames;
	uint64_t in_avail_frames;

	if (!_sound.configured || _sound.frame_bytes == 0 || _sound.pcm_cfg.rate <= 0) {
		return 0;
	}

	out_avail_frames = dma_chain_avail_bytes() / SOUND_HW_FRAME_BYTES;
	pending_frames = audio_pending_output_frames_locked();
	if (pending_frames >= out_avail_frames) {
		return 0;
	}
	out_avail_frames -= pending_frames;
	in_avail_frames = ((uint64_t)out_avail_frames * (uint64_t)_sound.pcm_cfg.rate) /
			(uint64_t)SOUND_HW_RATE;
	return (uint32_t)MIN(MIN((uint64_t)_sound.buffer_bytes, (uint64_t)_sound.write_chunk_bytes),
			in_avail_frames * (uint64_t)_sound.frame_bytes);
}

static uint32_t audio_convert_to_hw_locked(const uint8_t* src, uint32_t size,
		uint8_t* dst, uint32_t out_frame_cap, uint32_t* consumed_bytes) {
	uint32_t input_frames;
	uint32_t consumed_frames = 0;
	uint32_t produced_frames = 0;

	*consumed_bytes = 0;
	if (_sound.frame_bytes == 0 || out_frame_cap == 0) {
		return 0;
	}

	input_frames = size / _sound.frame_bytes;
	while (produced_frames < out_frame_cap) {
		if (!_sound.hold_valid) {
			if (consumed_frames >= input_frames) {
				break;
			}
			audio_decode_frame_to_s16_stereo_locked(src + (consumed_frames * _sound.frame_bytes),
					&_sound.hold_left, &_sound.hold_right);
			consumed_frames++;
			_sound.hold_valid = true;
			_sound.resample_accum += SOUND_HW_RATE;
		}

		if (_sound.resample_accum < (uint32_t)_sound.pcm_cfg.rate) {
			_sound.hold_valid = false;
			continue;
		}

		audio_store_hw_frame(dst + (produced_frames * SOUND_HW_FRAME_BYTES),
				_sound.hold_left, _sound.hold_right);
		produced_frames++;
		_sound.resample_accum -= (uint32_t)_sound.pcm_cfg.rate;
		if (_sound.resample_accum < (uint32_t)_sound.pcm_cfg.rate) {
			_sound.hold_valid = false;
		}
	}

	*consumed_bytes = consumed_frames * _sound.frame_bytes;
	return produced_frames * SOUND_HW_FRAME_BYTES;
}

static void audio_hw_free_locked(void) {
	dma_chain_reset();
	memset(&_sound.pcm_cfg, 0, sizeof(_sound.pcm_cfg));
	_sound.sample_bytes = 0;
	_sound.frame_bytes = 0;
	_sound.period_bytes = 0;
	_sound.buffer_bytes = 0;
	_sound.write_chunk_bytes = 0;
	audio_reset_convert_state_locked();
	_sound.configured = false;
	_sound.prepared = false;
	_sound.started = false;
}

static int audio_hw_params_locked(const struct pcm_config* cfg) {
	struct pcm_config normalized;
	uint32_t sample_bytes;

	/* #region debug-point xgo-pcm-open-hw-params */
	if (cfg == NULL) {
		slog("xgo-soundd: hw_params cfg=null\n");
		return -1;
	}

	memcpy(&normalized, cfg, sizeof(normalized));
	if (normalized.bit_depth <= 0) {
		normalized.bit_depth = SOUND_DEFAULT_BIT_DEPTH;
	}
	if (normalized.rate <= 0) {
		normalized.rate = SOUND_DEFAULT_RATE;
	}
	if (normalized.channels <= 0) {
		normalized.channels = SOUND_DEFAULT_CHANNELS;
	}
	if (normalized.period_size <= 0) {
		normalized.period_size = SOUND_DEFAULT_PERIOD_SIZE;
	}
	if (normalized.period_count <= 0) {
		normalized.period_count = SOUND_DEFAULT_PERIOD_COUNT;
	}
	if (normalized.start_threshold <= 0) {
		normalized.start_threshold = normalized.period_size;
	}
	if (normalized.stop_threshold <= 0) {
		normalized.stop_threshold = normalized.period_size * normalized.period_count;
	}
	if (normalized.start_threshold > normalized.stop_threshold) {
		normalized.start_threshold = normalized.stop_threshold;
	}

	slog("xgo-soundd: hw_params raw bits=%d rate=%d ch=%d period=%d count=%d start=%d stop=%d normalized bits=%d rate=%d ch=%d period=%d count=%d start=%d stop=%d\n",
			cfg->bit_depth, cfg->rate, cfg->channels, cfg->period_size, cfg->period_count,
			cfg->start_threshold, cfg->stop_threshold,
			normalized.bit_depth, normalized.rate, normalized.channels,
			normalized.period_size, normalized.period_count,
			normalized.start_threshold, normalized.stop_threshold);

	if (normalized.bit_depth != 8 && normalized.bit_depth != 16 &&
			normalized.bit_depth != 24 && normalized.bit_depth != 32) {
		slog("xgo-soundd: hw_params reject unsupported bit_depth=%d\n", normalized.bit_depth);
		return -1;
	}
	if (normalized.rate < 8000 || normalized.rate > 96000) {
		slog("xgo-soundd: hw_params reject unsupported rate=%d\n", normalized.rate);
		return -1;
	}
	if (normalized.channels < 1 || normalized.channels > 2) {
		slog("xgo-soundd: hw_params reject unsupported channels=%d\n", normalized.channels);
		return -1;
	}
	if (normalized.period_size <= 0 || normalized.period_count <= 0) {
		slog("xgo-soundd: hw_params reject invalid period=%d count=%d\n",
				normalized.period_size, normalized.period_count);
		return -1;
	}

	sample_bytes = audio_sample_bytes(normalized.bit_depth);
	if (sample_bytes == 0) {
		slog("xgo-soundd: hw_params reject sample_bytes=0 for bit_depth=%d\n",
				normalized.bit_depth);
		return -1;
	}

	audio_hw_free_locked();
	memcpy(&_sound.pcm_cfg, &normalized, sizeof(normalized));
	_sound.sample_bytes = sample_bytes;
	_sound.frame_bytes = (uint32_t)normalized.channels * sample_bytes;
	_sound.period_bytes = (uint32_t)normalized.period_size * _sound.frame_bytes;
	_sound.buffer_bytes = _sound.period_bytes * (uint32_t)normalized.period_count;
	_sound.write_chunk_bytes = _sound.buffer_bytes;
	_sound.configured = true;
	slog("xgo-soundd: hw_params ok frame=%u period_bytes=%u buffer_bytes=%u\n",
			_sound.frame_bytes, _sound.period_bytes, _sound.buffer_bytes);
	/* #endregion debug-point xgo-pcm-open-hw-params */
	return 0;
}

static int audio_ensure_default_config_locked(void) {
	struct pcm_config cfg;

	if (_sound.configured) {
		/* #region debug-point xgo-pcm-open-default-config */
		slog("xgo-soundd: default_config skip existing rate=%d ch=%d bits=%d period=%d count=%d\n",
				_sound.pcm_cfg.rate, _sound.pcm_cfg.channels, _sound.pcm_cfg.bit_depth,
				_sound.pcm_cfg.period_size, _sound.pcm_cfg.period_count);
		/* #endregion debug-point xgo-pcm-open-default-config */
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
	/* #region debug-point xgo-pcm-open-default-config */
	slog("xgo-soundd: default_config apply bits=%d rate=%d ch=%d period=%d count=%d start=%d stop=%d\n",
			cfg.bit_depth, cfg.rate, cfg.channels, cfg.period_size, cfg.period_count,
			cfg.start_threshold, cfg.stop_threshold);
	/* #endregion debug-point xgo-pcm-open-default-config */
	return audio_hw_params_locked(&cfg);
}

static int audio_prepare_locked(void) {
	if (!_sound.configured) {
		/* #region debug-point xgo-pcm-open-prepare */
		slog("xgo-soundd: prepare reject configured=0\n");
		/* #endregion debug-point xgo-pcm-open-prepare */
		return -1;
	}

	dma_chain_reset();
	audio_reset_convert_state_locked();
	_sound.prepared = true;
	_sound.started = false;
	/* #region debug-point xgo-pcm-open-prepare */
	slog("xgo-soundd: prepare ok pid=%d frame=%u rate=%d ch=%d bits=%d\n",
			_sound.occupied_pid, _sound.frame_bytes, _sound.pcm_cfg.rate,
			_sound.pcm_cfg.channels, _sound.pcm_cfg.bit_depth);
	/* #endregion debug-point xgo-pcm-open-prepare */
	return 0;
}

static int audio_start_locked(void) {
	if (!_sound.prepared) {
		return -1;
	}

	_sound.started = true;
	return 0;
}

static int audio_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(node);
	UNUSED(buf);
	UNUSED(size);
	UNUSED(offset);
	UNUSED(p);

	return VFS_ERR_RETRY;
}

static int sound_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, int oflag, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(info);
	UNUSED(oflag);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	pthread_mutex_lock(&_sound_lock);
	/* #region debug-point xgo-pcm-open-open */
	slog("xgo-soundd: open request pid=%d open_count=%d occupied=%d oflag=%d\n",
			from_pid, _sound.open_count, _sound.occupied_pid, oflag);
	/* #endregion debug-point xgo-pcm-open-open */
	if (_sound.open_count > 0 && _sound.occupied_pid != from_pid) {
		/* #region debug-point xgo-pcm-open-open */
		slog("xgo-soundd: open reject pid=%d occupied=%d open_count=%d\n",
				from_pid, _sound.occupied_pid, _sound.open_count);
		/* #endregion debug-point xgo-pcm-open-open */
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	if (_sound.open_count == 0) {
		audio_hw_free_locked();
	}
	_sound.occupied_pid = from_pid;
	_sound.open_count++;
	/* #region debug-point xgo-pcm-open-open */
	slog("xgo-soundd: open ok pid=%d open_count=%d configured=%d prepared=%d started=%d\n",
			from_pid, _sound.open_count, _sound.configured ? 1 : 0,
			_sound.prepared ? 1 : 0, _sound.started ? 1 : 0);
	/* #endregion debug-point xgo-pcm-open-open */
	pthread_mutex_unlock(&_sound_lock);
	return 0;
}

static int sound_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* info, void* p) {
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(node);
	UNUSED(info);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	pthread_mutex_lock(&_sound_lock);
	/* #region debug-point xgo-pcm-open-close */
	slog("xgo-soundd: close request pid=%d open_count=%d occupied=%d\n",
			from_pid, _sound.open_count, _sound.occupied_pid);
	/* #endregion debug-point xgo-pcm-open-close */
	if (_sound.occupied_pid != from_pid || _sound.open_count <= 0) {
		/* #region debug-point xgo-pcm-open-close */
		slog("xgo-soundd: close reject pid=%d open_count=%d occupied=%d\n",
				from_pid, _sound.open_count, _sound.occupied_pid);
		/* #endregion debug-point xgo-pcm-open-close */
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}

	_sound.open_count--;
	if (_sound.open_count == 0) {
		audio_hw_free_locked();
		_sound.occupied_pid = 0;
	}
	/* #region debug-point xgo-pcm-open-close */
	slog("xgo-soundd: close ok pid=%d open_count=%d occupied=%d\n",
			from_pid, _sound.open_count, _sound.occupied_pid);
	/* #endregion debug-point xgo-pcm-open-close */
	pthread_mutex_unlock(&_sound_lock);
	return 0;
}

static int sound_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	const uint8_t* src;
	uint8_t converted[SOUND_CONVERT_BUF_BYTES];
	int total = 0;

	UNUSED(dev);
	UNUSED(fd);
	UNUSED(node);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	if (size <= 0) {
		return -1;
	}
	if (offset < 0 || offset >= size) {
		return 0;
	}

	pthread_mutex_lock(&_sound_lock);
	if (_sound.occupied_pid != from_pid) {
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	if (!_sound.configured && audio_ensure_default_config_locked() != 0) {
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	if (!_sound.prepared && audio_prepare_locked() != 0) {
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	if (!_sound.started && audio_start_locked() != 0) {
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	if (_sound.frame_bytes == 0) {
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}
	pthread_mutex_unlock(&_sound_lock);

	size -= offset;
	size = (size / (int)_sound.frame_bytes) * (int)_sound.frame_bytes;
	if (size == 0) {
		return 0;
	}

	src = (const uint8_t*)buf + offset;
	while (total < size) {
		uint32_t input_avail;
		uint32_t output_avail_frames;
		uint32_t consumed_bytes;
		uint32_t produced_bytes;
		int chunk_bytes;
		int pushed;

		pthread_mutex_lock(&_sound_lock);
		if (_sound.occupied_pid != from_pid || !_sound.started) {
			pthread_mutex_unlock(&_sound_lock);
			return total > 0 ? total : -1;
		}

		input_avail = audio_input_avail_bytes_locked();
		chunk_bytes = size - total;
		if ((uint32_t)chunk_bytes > input_avail) {
			chunk_bytes = (int)input_avail;
		}
		chunk_bytes = (chunk_bytes / (int)_sound.frame_bytes) * (int)_sound.frame_bytes;
		output_avail_frames = dma_chain_avail_bytes() / SOUND_HW_FRAME_BYTES;
		if (output_avail_frames > SOUND_CONVERT_BUF_FRAMES) {
			output_avail_frames = SOUND_CONVERT_BUF_FRAMES;
		}
		if (_sound.hold_valid && output_avail_frames > 0) {
			produced_bytes = audio_convert_to_hw_locked(NULL, 0, converted,
					output_avail_frames, &consumed_bytes);
			if (produced_bytes != 0) {
				pushed = dma_chain_push(converted, (int)produced_bytes);
				if (pushed != (int)produced_bytes) {
					pthread_mutex_unlock(&_sound_lock);
					return total > 0 ? total : -1;
				}
				pthread_mutex_unlock(&_sound_lock);
				continue;
			}
		}
		if (chunk_bytes > 0 && output_avail_frames > 0) {
			produced_bytes = audio_convert_to_hw_locked(src + total, (uint32_t)chunk_bytes,
					converted, output_avail_frames, &consumed_bytes);
			if (consumed_bytes > 0) {
				if (produced_bytes != 0) {
					pushed = dma_chain_push(converted, (int)produced_bytes);
					if (pushed != (int)produced_bytes) {
						pthread_mutex_unlock(&_sound_lock);
						return total > 0 ? total : -1;
					}
				}
				total += (int)consumed_bytes;
				pthread_mutex_unlock(&_sound_lock);
				continue;
			}
		}
		pthread_mutex_unlock(&_sound_lock);
		proc_usleep(SOUND_WRITE_SLEEP_US);
	}

	return total;
}

static uint32_t sound_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
	uint32_t events = 0;

	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(p);

	pthread_mutex_lock(&_sound_lock);
	if (_sound.configured &&
			_sound.frame_bytes != 0 &&
			audio_input_avail_bytes_locked() >= _sound.frame_bytes) {
		events = VFS_EVT_WR;
	}
	pthread_mutex_unlock(&_sound_lock);
	return events;
}

static int sound_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	int result = 0;
	struct pcm_config cfg;

	UNUSED(dev);
	UNUSED(p);

	from_pid = proc_getpid(from_pid);
	pthread_mutex_lock(&_sound_lock);
	/* #region debug-point xgo-pcm-open-dev-cntl */
	slog("xgo-soundd: dev_cntl enter pid=%d cmd=%d occupied=%d configured=%d prepared=%d started=%d\n",
			from_pid, cmd, _sound.occupied_pid,
			_sound.configured ? 1 : 0,
			_sound.prepared ? 1 : 0,
			_sound.started ? 1 : 0);
	/* #endregion debug-point xgo-pcm-open-dev-cntl */
	if (_sound.occupied_pid != from_pid) {
		/* #region debug-point xgo-pcm-open-dev-cntl */
		slog("xgo-soundd: dev_cntl reject pid=%d occupied=%d cmd=%d\n",
				from_pid, _sound.occupied_pid, cmd);
		/* #endregion debug-point xgo-pcm-open-dev-cntl */
		pthread_mutex_unlock(&_sound_lock);
		return -1;
	}

	switch (cmd) {
	case CTRL_PCM_DEV_HW:
		memset(&cfg, 0, sizeof(cfg));
		proto_read_to(in, &cfg, sizeof(cfg));
		result = audio_hw_params_locked(&cfg);
		break;
	case CTRL_PCM_DEV_HW_FREE:
		audio_hw_free_locked();
		result = 0;
		break;
	case CTRL_PCM_DEV_PRPARE:
		result = audio_prepare_locked();
		break;
	case CTRL_PCM_BUF_AVAIL:
		if (!_sound.configured && audio_ensure_default_config_locked() != 0) {
			result = -1;
		}
		else if (_sound.buffer_bytes == 0 || _sound.write_chunk_bytes == 0) {
			result = -1;
		}
		else {
			result = (int)audio_input_avail_bytes_locked();
		}
		break;
	default:
		result = -1;
		break;
	}

	/* #region debug-point xgo-pcm-open-dev-cntl */
	slog("xgo-soundd: dev_cntl exit pid=%d cmd=%d result=%d configured=%d prepared=%d started=%d\n",
			from_pid, cmd, result,
			_sound.configured ? 1 : 0,
			_sound.prepared ? 1 : 0,
			_sound.started ? 1 : 0);
	/* #endregion debug-point xgo-pcm-open-dev-cntl */
	pthread_mutex_unlock(&_sound_lock);
	PF->addi(ret, result);
	return 0;
}

static int sound_loop(vdevice_t* dev, void* p) {
	bool wake_write = false;
	uint8_t converted[SOUND_CONVERT_BUF_BYTES];

	UNUSED(p);

	pthread_mutex_lock(&_sound_lock);
	ipc_disable();
	dma_chain_flush();
	ipc_enable();
	if (_sound.started && _sound.hold_valid) {
		uint32_t output_avail_frames = dma_chain_avail_bytes() / SOUND_HW_FRAME_BYTES;
		uint32_t consumed_bytes = 0;
		uint32_t produced_bytes;

		if (output_avail_frames > SOUND_CONVERT_BUF_FRAMES) {
			output_avail_frames = SOUND_CONVERT_BUF_FRAMES;
		}
		if (output_avail_frames > 0) {
			produced_bytes = audio_convert_to_hw_locked(NULL, 0, converted,
					output_avail_frames, &consumed_bytes);
			if (produced_bytes != 0) {
				dma_chain_push(converted, (int)produced_bytes);
			}
		}
	}
	if (_sound.configured &&
			_sound.frame_bytes != 0 &&
			audio_input_avail_bytes_locked() >= _sound.frame_bytes) {
		wake_write = true;
	}
	pthread_mutex_unlock(&_sound_lock);

	if (wake_write) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
	}
	proc_usleep(SOUND_WRITE_SLEEP_US);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/sound0";
	vdevice_t dev;

	_mmio_base = mmio_map();
	pthread_mutex_init(&_sound_lock, NULL);

	if (wm8960_init() != 0) {
		return -1;
	}
	pcm_init();
	dma_chain_init();

	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "audio");
	dev.open = sound_open;
	dev.close = sound_close;
	dev.read = audio_read;
	dev.write = sound_write;
	dev.dev_cntl = sound_dev_cntl;
	dev.check_poll_events = sound_check_poll_events;
	dev.loop_step = sound_loop;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
	return 0;
}
