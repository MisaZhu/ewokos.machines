#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>

#include "i2s.h"
#include "wm8960.h"
#include "dma_chain.h"

static int audio_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, 
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)node;
	(void)size;
	(void)p;

	return VFS_ERR_RETRY;
}

static int audio_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)node;
	(void)from_pid;
	(void)offset;
	(void)p;

	size = dma_chain_push(buf, size); 
	return size?size:VFS_ERR_RETRY;
}

static int loop(vdevice_t* dev, void* p) {
	(void)dev;
	(void)p;

	ipc_disable();
	dma_chain_flush();
	ipc_enable();
	vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);	
	proc_usleep(100);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/sound";
	_mmio_base = mmio_map();

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "audio");

	dev.read = audio_read;
	dev.write = audio_write;
	dev.loop_step = loop;

	wm8960_init();
	pcm_init();
	dma_chain_init();
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	return 0;
}
