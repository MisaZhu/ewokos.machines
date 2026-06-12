
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <ewoksys/vdevice.h>

#include "list.h"
#include "pcm_lib.h"
#include "miyoo-dais.h"

#define UNUSED(p) ((void)p)

static struct snd_card *sound_card = 0;

static int enter_pcm_device_loop(struct snd_pcm *pcm, const char* dev_name)
{
	if (pcm == NULL) {
		return -1;
	}

	vdevice_t *vdev = (vdevice_t*)pcm->private_data;
	if (vdev == NULL || dev_name == NULL || dev_name[0] == '\0') {
		return -1;
	}
	char mount_point[33] = {0};
	/*
	 * snprintf always terminates, strncpy here can leave the buffer
	 * without a NUL when the source length is >= 32, which then makes
	 * the VFS layer walk past the buffer when registering the mount.
	 */
	snprintf(mount_point, sizeof(mount_point), "%s", dev_name);
	return device_run(vdev, mount_point, FS_TYPE_CHAR, 0666);
}

int main(int argc, char *argv[])
{
	const char* dev_name = argc < 2 ? "/dev/sound0":argv[1];
	int ret = 0;
	ret = snd_card_new(&sound_card, "ewokos sound card");
	if (ret != 0 || !sound_card) {
		return 0;
	}

	struct snd_pcm *pcm_playback;
	ret = snd_pcm_new(sound_card, PCM_TYPE_PLAYBACK, 0, &pcm_playback);
	if (ret != 0) {
		snd_card_free(sound_card);
		return -1;
	}

	/* Add msc313 sound card dais on PCM */
	ret = msc313_add_dais(pcm_playback);
	if (ret != 0) {
		snd_card_free(sound_card);
		return -1;
	}
	/* This is Soc sound card, contain DAIs */
	ret = snd_set_pcm_ops(pcm_playback, &soc_dai_pcm_ops);
	if (ret != 0) {
		snd_card_free(sound_card);
		return -1;
	}

	ret = snd_card_register(sound_card);
	if (ret != 0) {
		snd_card_free(sound_card);
		return -1;
	}

	snd_card_info_print(sound_card);

	ret = enter_pcm_device_loop(pcm_playback, dev_name);
	if (ret != 0) {
		snd_card_free(sound_card);
		return -1;
	}

	return 0;
}
