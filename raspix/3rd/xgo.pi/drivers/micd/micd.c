#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <ewoksys/charbuf.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

#include <arch/bcm283x/i2s.h>

#define MIC_READ_RAW_BYTES 256
#define MIC_FRAME_IN_BYTES 8
#define MIC_FRAME_OUT_BYTES 4

static charbuf_t* _mic_buf;
static int _mic_shift = -1;

static int16_t mic_to_s16(int32_t sample) {
	int shift = _mic_shift;

	if (shift < 0 && sample != 0) {
		shift = ((sample & 0xFF) == 0) ? 16 : 8;
		_mic_shift = shift;
	}
	if (shift < 0) {
		shift = 8;
	}
	return (int16_t)(sample >> shift);
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

static int mic_loop(vdevice_t* dev, void* p) {
	uint8_t raw[MIC_READ_RAW_BYTES];
	uint8_t chunk[MIC_READ_RAW_BYTES / 2];
	int rd;
	int pcm_bytes;

	(void)p;
	rd = pcm_read(raw, sizeof(raw));
	if (rd <= 0) {
		proc_usleep(2000);
		return 0;
	}
	pcm_bytes = mic_convert_frames(raw, rd, chunk, sizeof(chunk));
	if (pcm_bytes <= 0) {
		proc_usleep(2000);
		return 0;
	}

	ipc_disable();
	for (int i = 0; i < pcm_bytes; i++) {
		charbuf_push(_mic_buf, chunk[i], true);
	}
	ipc_enable();

	vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/mic0";
	vdevice_t dev;

	_mmio_base = mmio_map();
	pcm_init_inmp441();

	_mic_buf = charbuf_new(0);
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "xgo-mic");
	dev.read = mic_read;
	dev.check_poll_events = mic_check_poll_events;
	dev.loop_step = mic_loop;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
