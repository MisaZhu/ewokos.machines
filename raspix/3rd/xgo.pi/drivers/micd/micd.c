#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <ewoksys/charbuf.h>
#include <ewoksys/ipc.h>
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

#include <arch/bcm283x/i2s.h>
#include "../soundd/wm8960.h"

#define MIC_READ_RAW_BYTES 256
#define MIC_FRAME_IN_BYTES 8
#define MIC_FRAME_OUT_BYTES 4

static charbuf_t* _mic_buf;
static int _mic_clients = 0;
static uint64_t _last_wake_ms = 0;

/* diagnostics: dumped once per second by mic_dbg_tick() */
static uint32_t _dbg_loops;
static uint32_t _dbg_reads;
static uint32_t _dbg_bytes;
static uint32_t _dbg_nonzero;
static uint64_t _dbg_last_ms;
static char _dbg_line[128] = "no stats yet";

static void mic_dbg_tick(int rd, const uint8_t* raw) {
	uint64_t now;

	_dbg_loops++;
	if (rd > 0) {
		const uint32_t* w = (const uint32_t*)raw;
		_dbg_reads++;
		_dbg_bytes += (uint32_t)rd;
		for (int i = 0; i < rd / 4; i++) {
			if (w[i] != 0 && w[i] != 0xffffffffu) {
				_dbg_nonzero++;
			}
		}
	}

	now = kernel_tic_ms(0);
	if (now - _dbg_last_ms < 1000) {
		return;
	}
	_dbg_last_ms = now;
	snprintf(_dbg_line, sizeof(_dbg_line),
			"cs=%08x cm=%08x\nlp=%u rd=%u by=%u nz=%u",
			*((volatile uint32_t*)(uintptr_t)ARM_PCM_CS_A),
			*((volatile uint32_t*)(uintptr_t)(CM_BASE + CM_I2SCTL)),
			_dbg_loops, _dbg_reads, _dbg_bytes, _dbg_nonzero);
	klog("micd: %s\n", _dbg_line);
	_dbg_loops = 0;
	_dbg_reads = 0;
	_dbg_bytes = 0;
	_dbg_nonzero = 0;
}

static int mic_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	(void)dev;
	(void)from_pid;
	(void)cmd;
	(void)in;
	(void)p;

	PF->adds(ret, _dbg_line);
	return 0;
}

static int16_t mic_to_s16(int32_t sample) {
	/* 16-bit slots: each FIFO word holds one sign-extended s16 sample */
	return (int16_t)sample;
}

static int mic_convert_frames(const uint8_t* raw, int raw_bytes, uint8_t* out, int out_cap) {
	const int32_t* in = (const int32_t*)raw;
	int in_words;
	int out_bytes = 0;

	if (raw == NULL || out == NULL || raw_bytes < MIC_FRAME_IN_BYTES) {
		return 0;
	}

	in_words = raw_bytes / (int)sizeof(int32_t);
	for (int i = 0; i + 1 < in_words; i += 2) {
		int16_t left;
		int16_t right;

		if (out_bytes + MIC_FRAME_OUT_BYTES > out_cap) {
			break;
		}

		left = mic_to_s16(in[i]);
		right = mic_to_s16(in[i + 1]);
		out[out_bytes + 0] = (uint8_t)(left & 0xFF);
		out[out_bytes + 1] = (uint8_t)(((uint16_t)left >> 8) & 0xFF);
		out[out_bytes + 2] = (uint8_t)(right & 0xFF);
		out[out_bytes + 3] = (uint8_t)(((uint16_t)right >> 8) & 0xFF);
		out_bytes += MIC_FRAME_OUT_BYTES;
	}
	return out_bytes;
}

static int mic_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	int i;
	char* out = (char*)buf;

	for (i = 0; i < size; i++) {
		if (charbuf_pop(_mic_buf, out + i) != 0) {
			break;
		}
	}
	return i == 0 ? VFS_ERR_RETRY : i;
}

static uint32_t mic_check_poll_events(vdevice_t* dev, int fd, int from_pid,
		fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	if (!charbuf_is_empty(_mic_buf)) {
		return VFS_EVT_RD;
	}
	return 0;
}

static int mic_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, int oflag, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)oflag;
	(void)p;

	if (_mic_clients == 0) {
		charbuf_clear(_mic_buf); /* drop stale audio from before */
	}
	_mic_clients++;
	return 0;
}

static int mic_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* fsinfo, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)fsinfo;
	(void)p;

	if (_mic_clients > 0) {
		_mic_clients--;
	}
	return 0;
}

static int mic_loop(vdevice_t* dev, void* p) {
	uint8_t raw[MIC_READ_RAW_BYTES];
	uint8_t chunk[MIC_READ_RAW_BYTES / 2];
	int rd;
	int pcm_bytes;
	uint64_t now;

	(void)p;

	/* nobody listening: keep quiet and stay away from vfsd */
	if (_mic_clients <= 0) {
		pcm_read(raw, sizeof(raw)); /* drain FIFO so it doesn't sit in overrun */
		proc_usleep(20000);
		return 0;
	}

	rd = pcm_read(raw, sizeof(raw));
	mic_dbg_tick(rd, raw);
	if (rd > 0) {
		pcm_bytes = mic_convert_frames(raw, rd, chunk, sizeof(chunk));
		if (pcm_bytes > 0) {
			ipc_disable();
			for (int i = 0; i < pcm_bytes; i++) {
				charbuf_push(_mic_buf, chunk[i], true);
			}
			ipc_enable();

			/* throttle wakeups: one IPC per ~10ms is plenty for readers */
			now = kernel_tic_ms(0);
			if (now - _last_wake_ms >= 10) {
				_last_wake_ms = now;
				vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
			}
		}
	}
	/* pace to the data rate instead of spinning on a hot FIFO */
	proc_usleep(1000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/mic0";
	vdevice_t dev;

	_mmio_base = mmio_map();
	/*
	 * soundd owns the codec and the PCM block when it runs first
	 * (its pcm_init enables both TX and RX with the same 16-bit
	 * frame). Only init hardware here when nobody did it yet, so
	 * both daemons can coexist without clobbering each other.
	 */
	if (!pcm_rx_active()) {
		if (wm8960_init() != 0) {
			return -1;
		}
		pcm_init_rx16();
	}

	_mic_buf = charbuf_new(0);
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "xgo-mic");
	dev.open = mic_open;
	dev.close = mic_close;
	dev.read = mic_read;
	dev.dev_cntl = mic_dev_cntl;
	dev.check_poll_events = mic_check_poll_events;
	dev.loop_step = mic_loop;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
