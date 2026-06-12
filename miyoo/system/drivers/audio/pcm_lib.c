#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <ewoksys/klog.h>
#include <ewoksys/vfs.h>
#include <sys/errno.h>

#include "pcm_lib.h"

#define KLOG 		klog
#define UNUSED(v)	((void)v)
//define EAGAIN		1 /* defined in errno.h */
#define ENOMEM		12
#define	EINVAL		22
#define ETIMEDOUT	23
#define	EBADF		9
#define	EPIPE		32
#define	EBUSY		16

#define WRITE_LONG(a, v)	(*(volatile int32_t *)(a) = (v))
#define READ_LONG(a)		(*(volatile int32_t *)(a))

#define TIME_USEC_TO_FRAMES(usec, rate) (uint32_t)((uint64_t)((usec) * (rate)) / (uint64_t)1000000)
#define SOUND_DEFAULT_BIT_DEPTH		16
#define SOUND_DEFAULT_RATE		48000
#define SOUND_DEFAULT_CHANNELS		2
#define SOUND_DEFAULT_PERIOD_SIZE	1024
#define SOUND_DEFAULT_PERIOD_COUNT	4
#define ALOG(...) slog("aud: " __VA_ARGS__)

int snd_card_new(struct snd_card **snd_card, const char *name)
{
	struct snd_card *card;
	card = (struct snd_card *)malloc(sizeof(*card));
	if (card == NULL) {
		return -ENOMEM;
	}

	memset(card, 0, sizeof(struct snd_card));
	list_init(&card->dev_list);
	list_init(&card->pcm_list);
	strncpy(card->name, name, sizeof(card->name) - 1);
	card->name[sizeof(card->name) - 1] = '\0';
	*snd_card = card;
	return 0;
}

int snd_card_free(struct snd_card *card)
{
	if (card == NULL) {
		return -EBADF;
	}

	struct listnode *node;
	struct listnode *node_next;
	list_for_each_safe(node, node_next, &card->pcm_list) {
		struct snd_pcm *pcm = node_to_item(node, struct snd_pcm, list);
		snd_pcm_free(pcm);
	}
	return 0;
}

int snd_card_register(struct snd_card *card)
{
	struct listnode *node;
	list_for_each(node, &card->dev_list) {
		struct snd_device *cur_dev = node_to_item(node, struct snd_device, list);
		cur_dev->dev_new(cur_dev);
	}
	return 0;
}

int snd_card_unregister(struct snd_card *card)
{
	struct listnode *node;
	list_for_each(node, &card->dev_list) {
		struct snd_device *cur_dev = node_to_item(node, struct snd_device, list);
		cur_dev->dev_free(cur_dev);
	}
	return 0;
}

int snd_card_info_print(struct snd_card *card)
{
	if (card == NULL) {
		return 0;
	}
	KLOG("-Card Info:\n%s\n", card->name);
	KLOG("\tName:%s\n", card->name);
	KLOG("\tPcm Num:%d\n", card->num_pcm);
	KLOG("\t-PCM Info:\n");
	if (card->num_pcm == 0) {
		KLOG("\t\tNo PCM!");
	} else {
		struct listnode *node;
		int index = 0;
		list_for_each(node, &card->pcm_list) {
			struct snd_pcm_substream *substream = NULL;
			struct snd_pcm *pcm = node_to_item(node, struct snd_pcm, list);
			substream = pcm->substream;
			KLOG("\t\tid:%d\n", index);
			KLOG("\t\tname:%s\n", pcm->name);
			if (substream != NULL) {
				KLOG("\t\tsubstream:%s, status:%s\n", substream->name,
							pcm_state_str(substream->runtime->status.state));
			}
		}
	}
	return 0;
}

static int snd_pcm_substream_new(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	substream = malloc(sizeof(*substream));
	if (substream == NULL) {
		return -ENOMEM;
	}

	memset(substream, 0, sizeof(*substream));
	pcm->substream = substream;
	substream->pcm = pcm;
	strncpy(substream->name, "substream-0", 32);

	struct snd_pcm_runtime *runtime;
	runtime = malloc(sizeof(*runtime));
	if (runtime == NULL) {
		free(substream);
		return -ENOMEM;
	}

	pthread_mutex_init(&substream->lock, NULL);
	memset(runtime, 0, sizeof(*runtime));
	substream->runtime = runtime;
	return 0;
}

static int snd_pcm_substream_free(struct snd_pcm *pcm)
{
	if (pcm->substream == NULL) {
		return 0;
	}

	free(pcm->substream->runtime);
	pcm->substream->runtime = NULL;
	free(pcm->substream);
	pcm->substream = NULL;
	return 0;
}

int snd_pcm_lock(struct snd_pcm_substream *substream)
{
	pthread_mutex_lock(&substream->lock);
	return 0;
}

int snd_pcm_unlock(struct snd_pcm_substream *substream)
{
	pthread_mutex_unlock(&substream->lock);
	return 0;
}


static int snd_pcm_device_create(struct snd_device *device);
static int snd_pcm_device_free(struct snd_device *device);
static struct file_operation vdev_ops;

int snd_pcm_new(struct snd_card *card, int type, int id, struct snd_pcm **rpcm)
{
	struct snd_pcm *pcm;
	pcm = (struct snd_pcm*)malloc(sizeof(*pcm));
	if (pcm == NULL) {
		return -ENOMEM;
	}

	memset(pcm, 0, sizeof(*pcm));

	struct snd_device *device = malloc(sizeof(struct snd_device));
	if (device == NULL) {
		free(pcm);
		return -ENOMEM;
	}
	device->owner = pcm;
	device->param = (void*)&vdev_ops;
	device->type = SND_DEV_TYPE_PCM;
	device->dev_new = snd_pcm_device_create;
	device->dev_free = snd_pcm_device_free;

	int err = snd_pcm_substream_new(pcm);
	if (err != 0) {
		free(device);
		free(pcm);
		return -EINVAL;
	}

	pcm->type = type;
	pcm->id = id;
	snprintf(pcm->name, 32, "%s%c%d", "pcm", PCM_TYPE_TO_TAG(pcm->type), pcm->id);
	pcm->card = card;
	card->num_pcm++;
	list_init(&pcm->list);
	list_init(&pcm->dai_list);
	list_add_tail(&card->pcm_list, &pcm->list);
	list_add_tail(&card->dev_list, &device->list);
	*rpcm = pcm;
	return 0;
}

int snd_pcm_free(struct snd_pcm *pcm)
{
	if (pcm == NULL) {
		return 0;
	}

	snd_pcm_substream_free(pcm);
	list_remove(&pcm->list);
	pcm->card->num_pcm--;
	free(pcm);
	return 0;
}

int snd_set_pcm_ops(struct snd_pcm *pcm, struct snd_pcm_ops *ops)
{
	pcm->substream->ops = ops;
	return 0;
}

int snd_add_dai(struct snd_pcm *pcm, struct snd_soc_dai *dai, int dai_type)
{
	if (dai_type == DAI_TYPE_MEMIF) {
		list_add_head(&pcm->dai_list, &dai->list);
		pcm->mem_dai = dai;
	} else {
		list_add_tail(&pcm->dai_list, &dai->list);
	}
	return 0;
}

/***************** DAI operation ******************/
static int snd_dai_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->open != NULL) {
			dai->ops->open(dai, substream);
		}
	}
	return 0;
}

static int snd_dai_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->close != NULL) {
			dai->ops->close(dai, substream);
		}
	}
	return 0;
}

static int snd_dai_pcm_hw_params(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->hw_params != NULL) {
			dai->ops->hw_params(dai, substream);
		}
	}
	return 0;
}

static int snd_dai_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->hw_free != NULL) {
			dai->ops->hw_free(dai, substream);
		}
	}
	return 0;
}

static int snd_dai_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->prepare != NULL) {
			dai->ops->prepare(dai, substream);
		}
	}
	return 0;
}

static int snd_dai_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops->trigger != NULL) {
			dai->ops->trigger(dai, substream, cmd);
		}
	}
	return 0;
}

static int snd_dai_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai = pcm->mem_dai;
	int pos;
	if (dai->ops->pointer) {
		pos = dai->ops->pointer(dai, substream);
	} else {
		pos = 0;
		KLOG("%s() Error! The DAI hasn't pointer() ops!\n", __func__);
	}
	return pos;
}

static int snd_dai_pcm_ack(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai = pcm->mem_dai;
	int ret = 0;
	if (dai->ops->ack) {
		ret = dai->ops->ack(dai, substream);
	}
	return ret;
}

/*
 * soc_dai_pcm kick: iterates the DAI list and asks each DAI to push
 * newly-acked bytes into the DMA engine. Implemented in miyoo-dais.c
 * as mi_cpu_dai_kick -> msc313_bach_queue_pending().
 */
static int snd_dai_pcm_kick(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai;
	struct listnode *node;
	list_for_each(node, &pcm->dai_list) {
		dai = node_to_item(node, struct snd_soc_dai, list);
		if (dai->ops && dai->ops->kick) {
			dai->ops->kick(dai, substream);
		}
	}
	return 0;
}

struct snd_pcm_ops soc_dai_pcm_ops = {
	.open = snd_dai_pcm_open,
	.close = snd_dai_pcm_close,
	.hw_params = snd_dai_pcm_hw_params,
	.hw_free = snd_dai_pcm_hw_free,
	.prepare = snd_dai_pcm_prepare,
	.trigger = snd_dai_pcm_trigger,
	.pointer = snd_dai_pcm_pointer,
	.ack = snd_dai_pcm_ack,
	.kick = snd_dai_pcm_kick,
};

/**************** substream operations ******************/
static inline void delay(int32_t count)
{
	while (count > 0) count--;
}

int wait_avail(struct snd_pcm_substream *substream, int *ravail)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int32_t avail = 0;
	int err = 0;

	while (1) {
		if (runtime->status.state == PCM_STATE_RUNNING && substream->ops->pointer) {
			update_hw_ptr(substream, 0);
		}
		avail = play_avail(runtime);
		if (avail >= runtime->period_size) {
			break;
		}

		usleep(1000);

		//check pcm state again
		switch (runtime->status.state) {
		case PCM_STATE_XRUN: {
			err = -EPIPE;
			} return err;
		case PCM_STATE_OPEN:
		case PCM_STATE_SETUP: {
			err = -EBADF;
			} return err;
		case PCM_STATE_STOPED:{
			continue;
			}
		default:
			break;
		}
	}

	*ravail = avail;
	return err;
}

void snd_dump_substream(struct snd_pcm_substream *substream, int is_interrupt)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	KLOG("-%s() %s appl:%d hw_ptr:%d avail:%d hw_base:%d\n", __func__,
		(is_interrupt? "[IRQ]" : "[NM]"),
       	runtime->status.appl_ptr,
       	runtime->status.hw_ptr,
       	play_avail(runtime),
       	runtime->hw_ptr_base);
}


int update_hw_ptr(struct snd_pcm_substream *substream, int is_interrupt)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int pos = 0;
	int old_hw_ptr, new_hw_ptr, hw_base;
	int delta = 0;
	int cross_boundary = 0;

#if 0 //TODO
	uint64_t now_usec;
	uint32_t d_Frames;
	kernel_tic(NULL, &now_usec);
	d_Frames = TIME_USEC_TO_FRAMES(now_usec - runtime->irq_usec, runtime->rate);
#endif

	old_hw_ptr = runtime->status.hw_ptr;
	if (substream->ops->pointer) {
		pos = substream->ops->pointer(substream);
	}

	if (pos >= runtime->buffer_size) {
		KLOG("[DBG] Invalid position: pos=%d\n", pos);
		pos = 0;
	}

	//KLOG("[DBG] Invalid position: pos=%d delay_frames:%u\n", pos, d_Frames);

	hw_base = runtime->hw_ptr_base;
	new_hw_ptr = hw_base + pos;

	if (is_interrupt) {
		if (new_hw_ptr < old_hw_ptr) {
			hw_base += runtime->buffer_size;
			if (hw_base >= runtime->boundary) {
				hw_base = 0;
				cross_boundary++;
			}
			new_hw_ptr = hw_base + pos;
		}
	} else {
		if ((new_hw_ptr < old_hw_ptr) &&
			(hw_base + runtime->buffer_size <= runtime->status.appl_ptr)) {
			hw_base += runtime->buffer_size;
			if (hw_base >= runtime->boundary) {
				hw_base = 0;
				cross_boundary++;
			}
			new_hw_ptr = hw_base + pos;
		}
	}

	delta = new_hw_ptr - old_hw_ptr;
	if (delta < 0) {
		delta += runtime->boundary;
	}

	if (delta >= runtime->buffer_size) {
		KLOG("%s() [IRQ Delay] Interrupt:%d delta:%d > bufsize:%d new_hw_ptr=%d, old_hw_ptr=%d\n",
			__func__, is_interrupt, delta, runtime->buffer_size, new_hw_ptr, old_hw_ptr);
	}

	WRITE_LONG(&runtime->hw_ptr_base, hw_base);
	WRITE_LONG(&(runtime->status.hw_ptr), new_hw_ptr);

	runtime->irq_count++;

#if 0
	KLOG("%s() irq_count:%d avail:%d appl:%d hw_ptr:%d hw_base:%d old_hw:%d pos:%d\n",
		__func__,runtime->irq_count, avail,
		 runtime->status.appl_ptr, runtime->status.hw_ptr, runtime->hw_ptr_base, old_hw_ptr, pos);
#endif

	/*
	 * Playback XRUN means the hardware has drained all queued frames, so the
	 * writable space has grown to the stop threshold. Using frames_ready() here
	 * is backwards: a healthy running stream with data queued can have a large
	 * ready count and would be spuriously stopped after a few good writes.
	 */
	if (runtime->status.state == PCM_STATE_RUNNING &&
		play_avail(runtime) >= runtime->stop_threshold) {
		substream->ops->trigger(substream, PCM_TRIGER_STOP);
		runtime->status.state = PCM_STATE_XRUN;

		KLOG("%s() [Warning] XRun happen! appl:%d hw_ptr:%d avail:%d ready:%d stop:%d\n",
			__func__, runtime->status.appl_ptr, runtime->status.hw_ptr,
			play_avail(runtime), frames_ready(runtime), runtime->stop_threshold);
	}

	return 0;
}

static int do_transfer(struct snd_pcm_runtime *runtime,
						int appl_off,
						const void *src_base,
						int off,
						int frames)
{
	volatile uint8_t *hw_app_ptr =
		(volatile uint8_t *)runtime->dma_area + frame_to_bytes(runtime, appl_off);
	const uint8_t *user_ptr =
		(const uint8_t *)src_base + frame_to_bytes(runtime, off);
	int bytes = frame_to_bytes(runtime, frames);
	int i;

	for (i = 0; i < bytes; i++) {
		hw_app_ptr[i] = user_ptr[i];
	}
	return 0;
}

int snd_pcm_substeam_write(struct snd_pcm_substream *substream, const void *source, int size)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int offset = 0;
	int avail = 0;
	int written = 0;
	int err = 0;

	snd_pcm_lock(substream);
	switch (runtime->status.state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		break;
	case PCM_STATE_XRUN:
		snd_pcm_unlock(substream);
		return -EPIPE;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}

#if 0
	if (runtime->status.state == PCM_STATE_RUNNING) {
		//KLOG(">>>>%s()\n",__func__);
		update_hw_ptr(substream, 0);
	}
#endif
	snd_pcm_unlock(substream);

	avail = play_avail(runtime);
	ALOG("pcm_write enter frames=%d avail=%d appl=%d hw=%d state=%s start=%d stop=%d buf=%d boundary=%d\n",
		size, avail, runtime->status.appl_ptr, runtime->status.hw_ptr,
		pcm_state_str(runtime->status.state),
		runtime->start_threshold, runtime->stop_threshold,
		runtime->buffer_size, runtime->boundary);
	while(size > 0) {
		int copy_frames = 0;
		int to_end = 0;
		int appl_ptr = 0;
		int app_offset = 0;
		//int old_appl = 0;

		if (avail == 0) {
			err = wait_avail(substream, &avail);
			if (err < 0) {
				ALOG("pcm_write wait_avail fail err=%d appl=%d hw=%d state=%s\n",
					err, runtime->status.appl_ptr, runtime->status.hw_ptr,
					pcm_state_str(runtime->status.state));
				KLOG("%s() No avail timeout, ret=%d\n", __func__, err);
				break;
			}
		}

		copy_frames = size > avail ? avail : size;
		to_end = runtime->buffer_size - runtime->status.appl_ptr % runtime->buffer_size;
		if (copy_frames > to_end) {
			copy_frames = to_end;
		}

		app_offset = runtime->status.appl_ptr % runtime->buffer_size;
		//old_appl = runtime->status.appl_ptr;

		do_transfer(runtime, app_offset, source, offset, copy_frames);
		ALOG("pcm_write copied frames=%d app_offset=%d avail=%d appl=%d hw=%d\n",
			copy_frames, app_offset, avail,
			runtime->status.appl_ptr, runtime->status.hw_ptr);
		appl_ptr = runtime->status.appl_ptr + copy_frames;
		if (appl_ptr >= runtime->boundary) {
			appl_ptr -= runtime->boundary;
		}
		offset += copy_frames;
		size -= copy_frames;
		written += copy_frames;
		avail -= copy_frames;

		//Acquire lock
		snd_pcm_lock(substream);

		WRITE_LONG(&(runtime->status.appl_ptr), appl_ptr);

		if ((runtime->status.state == PCM_STATE_PREPARE) && (frames_ready(runtime) >= runtime->start_threshold)) {
			int temp = substream->ops->trigger(substream, PCM_TRIGER_START);
			ALOG("pcm_write trigger ret=%d ready=%d appl=%d hw=%d state=%s\n",
				temp, frames_ready(runtime),
				runtime->status.appl_ptr, runtime->status.hw_ptr,
				pcm_state_str(runtime->status.state));
			if (temp == 0) {
				set_pcm_state(substream, PCM_STATE_RUNNING);
			}
		}

		if ((get_pcm_state(substream) == PCM_STATE_RUNNING) && substream->ops->ack) {
			substream->ops->ack(substream);
		}
		/*
		 * After acking the new bytes we must hand them off to BACH
		 * synchronously. Otherwise the next IRQ empty tail call will
		 * see pending_bytes=0 and the BACH inflight level stays at 0,
		 * which turns the next write into XRUN. The optional kick()
		 * hook is the platform driver's chance to do that push.
		 */
		if (substream->ops && substream->ops->kick) {
			substream->ops->kick(substream);
		}
		//Release lock
		snd_pcm_unlock(substream);
	}
	//update pcm state TODO
	ALOG("pcm_write exit err=%d written=%d appl=%d hw=%d state=%s avail=%d\n",
		err, written, runtime->status.appl_ptr, runtime->status.hw_ptr,
		pcm_state_str(runtime->status.state), play_avail(runtime));

	return (err < 0 ? err : written);
}

int snd_pcm_substream_read(struct snd_pcm_substream *substream, void *dest, int frames)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int offset = 0;
	int avail = 0;
	int read = 0;
	int err = 0;

	snd_pcm_lock(substream);
	switch (runtime->status.state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		break;
	case PCM_STATE_XRUN:
		snd_pcm_unlock(substream);
		return -EPIPE;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	snd_pcm_unlock(substream);

	avail = capture_avail(runtime);

	while (frames > 0) {
		int copy_frames = 0;
		int to_end = 0;
		int hw_ptr = 0;
		int hw_offset = 0;

		if (avail == 0) {
			err = -EAGAIN;
			break;
		}

		copy_frames = frames > avail ? avail : frames;
		to_end = runtime->buffer_size - runtime->status.hw_ptr % runtime->buffer_size;
		if (copy_frames > to_end) {
			copy_frames = to_end;
		}

		hw_offset = runtime->status.hw_ptr % runtime->buffer_size;

		char *hw_ptr_pos = runtime->dma_area + frame_to_bytes(runtime, hw_offset);
		char *user_ptr = (char *)dest + frame_to_bytes(runtime, offset);
		memcpy(user_ptr, hw_ptr_pos, frame_to_bytes(runtime, copy_frames));

		hw_ptr = runtime->status.hw_ptr + copy_frames;
		if (hw_ptr >= runtime->boundary) {
			hw_ptr -= runtime->boundary;
		}

		offset += copy_frames;
		frames -= copy_frames;
		read += copy_frames;
		avail -= copy_frames;

		snd_pcm_lock(substream);
		WRITE_LONG(&(runtime->status.hw_ptr), hw_ptr);
		snd_pcm_unlock(substream);
	}

	return (err < 0 ? err : read);
}

int snd_pcm_buf_avail(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int avail_bytes = 0;

	/*
	 * miyoo currently relies on a Timer0-driven polling path to advance hw_ptr.
	 * If that IRQ path stalls, userspace writes can block forever even when the
	 * hardware level register is still readable. Poll once here as a fallback.
	 */
	if (runtime->status.state == PCM_STATE_RUNNING && substream->ops->pointer) {
		update_hw_ptr(substream, 0);
	}

	snd_pcm_lock(substream);
	switch (runtime->status.state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
	case PCM_STATE_SETUP:
		break;
	case PCM_STATE_XRUN:
		snd_pcm_unlock(substream);
		return -EPIPE;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}

	avail_bytes = frame_to_bytes(runtime, play_avail(runtime));
    	snd_pcm_unlock(substream);

	return avail_bytes;
}

static int snd_pcm_open(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream = pcm->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;
	if (substream->open_count != 0) {
		KLOG("%s() device is busy! open_count=%d\n",
			__func__, substream->open_count);
		return -EBUSY;
	}

	substream->open_count = 1;

	/* TODO: attach substream to PCM dynamicly */
	memset(runtime, 0, sizeof(struct snd_pcm_runtime));

	runtime->status.state = PCM_STATE_OPEN;
	substream->private_data = substream->pcm->private_data;
	if (substream->ops->open != NULL) {
		err = substream->ops->open(substream);
	}

	if (err != 0) {
		set_pcm_state(substream, PCM_STATE_UNKOWN);
	}
	return 0;
}

static int snd_pcm_write1(struct snd_pcm *pcm,
		const void *data,
		int size,/* in bytes */
		int offset)
{
	struct snd_pcm_substream *substream  = pcm->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret = 0;

	if (!runtime->dma_area || runtime->dma_bytes <= 0) {
		ALOG("write1 invalid dma area=%x addr=%x bytes=%d\n",
			runtime->dma_area, runtime->dma_addr, runtime->dma_bytes);
		KLOG("%s() ERROR, invalid dma buffer area:%x addr:%x bytes:%d\n",
			__func__, runtime->dma_area, runtime->dma_addr, runtime->dma_bytes);
		return -EBADF;
	}

	if (size == 0 || offset != 0) {
		return 0;
	}
	ALOG("write1 bytes=%d frames=%d frame_size=%d dma_area=%x dma_addr=%x dma_bytes=%d state=%s\n",
		size, size / runtime->frame_size, runtime->frame_size,
		runtime->dma_area, runtime->dma_addr, runtime->dma_bytes,
		pcm_state_str(runtime->status.state));
	KLOG("[TRACE] snd_pcm_write1 bytes:%d frames:%d frame_size:%d dma_area:%x dma_addr:%x dma_bytes:%d state:%s\n",
		size, size / runtime->frame_size, runtime->frame_size,
		runtime->dma_area, runtime->dma_addr, runtime->dma_bytes,
		pcm_state_str(runtime->status.state));

	if (runtime->status.state == PCM_STATE_PREPARE ||
		runtime->status.state == PCM_STATE_RUNNING) {
		ret = snd_pcm_substeam_write(substream, data, size / runtime->frame_size);
		if (ret > 0) {
			ret = ret * runtime->frame_size;
		}
	}
	return ret;
}


static void dump_pcm_runtime(struct snd_pcm_runtime *runtime)
{
	if (runtime != NULL) {
		KLOG("runtime bit:%d rate:%d channels:%d period_size:%d period_cnt:%d start_thres:%d stop_thres:%d\n",
			runtime->bit_depth,
			runtime->rate,
			runtime->channels,
			runtime->period_size,
			runtime->periods,
			runtime->start_threshold,
			runtime->stop_threshold);
	}
}

static void snd_pcm_default_config(struct pcm_config *config)
{
	memset(config, 0, sizeof(*config));
	config->bit_depth = SOUND_DEFAULT_BIT_DEPTH;
	config->rate = SOUND_DEFAULT_RATE;
	config->channels = SOUND_DEFAULT_CHANNELS;
	config->period_size = SOUND_DEFAULT_PERIOD_SIZE;
	config->period_count = SOUND_DEFAULT_PERIOD_COUNT;
	config->start_threshold = config->period_size * 2;
	if (config->start_threshold > config->period_size * config->period_count) {
		config->start_threshold = config->period_size * config->period_count;
	}
	config->stop_threshold = config->period_size * config->period_count;
}

static int snd_pcm_hw_sw_parms(struct snd_pcm_substream* substream, struct pcm_config *config)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	switch(runtime->status.state) {
	case PCM_STATE_OPEN:
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
		break;
	default:
		KLOG("%s() ERROR return! beacuse of state=%s\n",
			__func__, pcm_state_str(runtime->status.state));
		return -EBADF;
	}

	runtime->bit_depth = config->bit_depth;
	runtime->rate = config->rate;
	runtime->channels = config->channels;
	runtime->period_size = config->period_size;
	runtime->periods = config->period_count;
	runtime->buffer_size = runtime->period_size * runtime->periods;
	runtime->start_threshold = config->start_threshold;
	runtime->stop_threshold = config->stop_threshold;
	runtime->frame_bits = runtime->channels * runtime->bit_depth;
	if (runtime->stop_threshold == 0) {
		runtime->stop_threshold = runtime->buffer_size;
	}
	runtime->frame_size = runtime->channels * runtime->bit_depth / 8;
	runtime->boundary = runtime->buffer_size;
	if (runtime->boundary == 0) {
		KLOG("%s() ERROR! Invalid config!\n", __func__);
		return -EINVAL;
	}
	if (runtime->periods > 1) {
		int safe_start = runtime->period_size * 2;
		if (safe_start > runtime->buffer_size) {
			safe_start = runtime->buffer_size;
		}
		if (runtime->start_threshold <= 0 || runtime->start_threshold < safe_start) {
			runtime->start_threshold = safe_start;
		}
	} else if (runtime->start_threshold <= 0) {
		runtime->start_threshold = runtime->period_size;
	}
	if (runtime->start_threshold > runtime->buffer_size) {
		runtime->start_threshold = runtime->buffer_size;
	}

	uint32_t temp = (uint32_t)runtime->buffer_size;
	while (temp * 2 <= (uint32_t)(0x7FFFFFFF - runtime->buffer_size)) {
		temp *= 2;
	}
	runtime->boundary = (int32_t)temp;

	if (substream->ops->hw_params) {
		err = substream->ops->hw_params(substream);
		if (err == 0) {
			set_pcm_state(substream, PCM_STATE_SETUP);
		}
	}

	if (err != 0) {
		if (substream->ops->hw_free) {
			substream->ops->hw_free(substream);
		}
		set_pcm_state(substream, PCM_STATE_OPEN);
	}

	dump_pcm_runtime(runtime);
	KLOG("[DEBUG] sound0 hw_sw_parms ret:%d state:%s rate:%d ch:%d period:%d periods:%d start:%d stop:%d frame:%d dma_bytes:%d\n",
		err, pcm_state_str(runtime->status.state), runtime->rate,
		runtime->channels, runtime->period_size, runtime->periods,
		runtime->start_threshold, runtime->stop_threshold,
		runtime->frame_size, runtime->dma_bytes);

	KLOG("%s() ret:%d state:%s\n", __func__, err, pcm_state_str(runtime->status.state));
	return err;
}

static int snd_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	switch(runtime->status.state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_STOPED:
		break;
	default:
		return -EBADF;
	}

	if (substream->ops->hw_free) {
		err = substream->ops->hw_free(substream);
	}

	set_pcm_state(substream, PCM_STATE_OPEN);
	return err;
}

static int snd_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	switch(runtime->status.state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_STOPED:
	case PCM_STATE_XRUN:
		break;
	case PCM_STATE_OPEN:
		return -EBADF;
	case PCM_STATE_RUNNING:
		return -EBUSY;
	default:
		return -EBADF;
	}

	if (!substream->ops->prepare) {
		KLOG("%s() ERROR! Driver hasn't prepare()\n", __func__);
		return -EBADF;
	}

	KLOG("[DEBUG] sound0 pcm_prepare entry state:%s rate:%d ch:%d period:%d periods:%d start:%d stop:%d frame:%d dma_bytes:%d appl:%d hw:%d\n",
		pcm_state_str(runtime->status.state), runtime->rate,
		runtime->channels, runtime->period_size, runtime->periods,
		runtime->start_threshold, runtime->stop_threshold,
		runtime->frame_size, runtime->dma_bytes,
		runtime->status.appl_ptr, runtime->status.hw_ptr);
	err = substream->ops->prepare(substream);
	if (err == 0) {
		/*
		 * prepare() represents a fresh playback start on this driver.
		 * Reusing stale hw/appl pointers from a previous XRUN/run makes the
		 * next ack() see a huge delta and queue bogus pending_bytes.
		 *
		 * However, callers like fdev_write() may invoke this multiple
		 * times while the state is already PREPARE (e.g. userland opens
		 * with pcm_prepare() and then triggers the first write). Wiping
		 * appl_ptr/hw_ptr/pending on the second call would discard the
		 * data userspace has already pushed through do_transfer(), which
		 * is what caused the "first write short: got=-1" regression.
		 * Only reset when we are NOT already in PREPARE.
		 */
		if (runtime->status.state != PCM_STATE_PREPARE) {
			runtime->hw_ptr_base = 0;
			runtime->status.hw_ptr = 0;
			runtime->status.appl_ptr = 0;
			runtime->ack_count = 0;
			runtime->irq_count = 0;
		}
		set_pcm_state(substream, PCM_STATE_PREPARE);
	} else {
		runtime->hw_ptr_base = 0;
		runtime->status.appl_ptr = runtime->status.hw_ptr = 0;
		set_pcm_state(substream, PCM_STATE_SETUP);
	}
	KLOG("[DEBUG] sound0 pcm_prepare exit err:%d state:%s dma_bytes:%d appl:%d hw:%d ack:%d irq:%d\n",
		err, pcm_state_str(runtime->status.state), runtime->dma_bytes,
		runtime->status.appl_ptr, runtime->status.hw_ptr,
		runtime->ack_count, runtime->irq_count);
	return err;
}

static int snd_pcm_ensure_default_hw(struct snd_pcm_substream *substream)
{
	struct pcm_config config;
	int state = get_pcm_state(substream);

	if (state != PCM_STATE_OPEN) {
		return 0;
	}

	snd_pcm_default_config(&config);
	return snd_pcm_hw_sw_parms(substream, &config);
}

static int snd_pcm_ensure_write_ready(struct snd_pcm_substream *substream)
{
	int err;
	int state = get_pcm_state(substream);

	err = snd_pcm_ensure_default_hw(substream);
	if (err != 0) {
		return err;
	}

	state = get_pcm_state(substream);
	switch (state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		return 0;
	case PCM_STATE_SETUP:
	case PCM_STATE_STOPED:
		return snd_pcm_prepare(substream);
	case PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADF;
	}
}

static int snd_pcm_ensure_query_ready(struct snd_pcm_substream *substream)
{
	int err;
	int state = get_pcm_state(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	err = snd_pcm_ensure_default_hw(substream);
	if (err != 0) {
		KLOG("[DEBUG] sound0 ensure_query_ready default_hw err:%d state:%s appl:%d hw:%d\n",
			err, pcm_state_str(runtime->status.state),
			runtime->status.appl_ptr, runtime->status.hw_ptr);
		return err;
	}

	state = get_pcm_state(substream);
	switch (state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		return 0;
	case PCM_STATE_XRUN:
		KLOG("[DEBUG] sound0 ensure_query_ready XRUN appl:%d hw:%d\n",
			runtime->status.appl_ptr, runtime->status.hw_ptr);
		return -EPIPE;
	default:
		KLOG("[DEBUG] sound0 ensure_query_ready bad-state:%s appl:%d hw:%d\n",
			pcm_state_str(state), runtime->status.appl_ptr,
			runtime->status.hw_ptr);
		return -EBADF;
	}
}

static int snd_pcm_release_substream(struct snd_pcm_substream *substream)
{
	int state;

	state = get_pcm_state(substream);
	if (state == PCM_STATE_RUNNING ||
		state == PCM_STATE_PREPARE) {
		if (substream->ops->trigger) {
			substream->ops->trigger(substream, PCM_TRIGER_STOP);
		}

		set_pcm_state(substream, PCM_STATE_STOPED);
		state = get_pcm_state(substream);
	}

	if (state == PCM_STATE_XRUN) {
		set_pcm_state(substream, PCM_STATE_STOPED);
		state = get_pcm_state(substream);
		KLOG("%s() change state:XRUN to STOPED!\n", __func__);
	}

	if (state == PCM_STATE_SETUP ||
		state == PCM_STATE_XRUN ||
		state ==  PCM_STATE_STOPED) {
		if (substream->ops->hw_free) {
			substream->ops->hw_free(substream);
		}

		set_pcm_state(substream, PCM_STATE_OPEN);
		state = get_pcm_state(substream);
	}

	if (state == PCM_STATE_OPEN) {
		if (substream->ops->close) {
			substream->ops->close(substream);
		}
		set_pcm_state(substream, PCM_STATE_UNKOWN);
	}

	return 0;
}

static int snd_pcm_release(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream = pcm->substream;

	snd_pcm_release_substream(substream);
	substream->open_count = 0;
	KLOG("%s()\n", __func__);
	return 0;
}


/***** File operations: Make vdevice conectted with PCM ***/
int fdev_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, int oflag, void* p)
{
	KLOG("fdev_open() fd:%d from:%d\n", fd, from_pid);
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(oflag);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	ALOG("open fd=%d from=%d pcm=%s state=%s\n",
		fd, from_pid, pcm->name, pcm_state_str(pcm->substream->runtime->status.state));
	// #region debug-point A:open-device
	KLOG("[DEBUG] sound0 open fd:%d from:%d pcm:%s state:%s\n",
		fd, from_pid, pcm->name, pcm_state_str(pcm->substream->runtime->status.state));
	// #endregion
	int err = snd_pcm_open(pcm);
	return err;
}


int fdev_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* info, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(node);
	UNUSED(info);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	int err = 0;
	ALOG("close fd=%d from=%d state=%s appl=%d hw=%d\n",
		fd, from_pid, pcm_state_str(pcm->substream->runtime->status.state),
		pcm->substream->runtime->status.appl_ptr,
		pcm->substream->runtime->status.hw_ptr);
	err = snd_pcm_release(pcm);
	KLOG("fdev_close() fd:%d from:%d\n", fd, from_pid);
	return err;
}


int fdev_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, const void* buf, int size, int offset, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	struct snd_pcm_substream *substream = pcm->substream;
	ALOG("write enter fd=%d size=%d offset=%d state=%s frame=%d appl=%d hw=%d\n",
		fd, size, offset, pcm_state_str(substream->runtime->status.state),
		substream->runtime->frame_size,
		substream->runtime->status.appl_ptr,
		substream->runtime->status.hw_ptr);
	// #region debug-point A:write-entry
	KLOG("[DEBUG] sound0 write entry size:%d offset:%d state:%s frame:%d appl:%d hw:%d\n",
		size, offset, pcm_state_str(substream->runtime->status.state),
		substream->runtime->frame_size,
		substream->runtime->status.appl_ptr,
		substream->runtime->status.hw_ptr);
	// #endregion
	int err = snd_pcm_ensure_write_ready(substream);
	if (err != 0) {
		ALOG("write ensure_ready fail err=%d state=%s\n",
			err, pcm_state_str(substream->runtime->status.state));
		// #region debug-point B:ensure-ready-fail
		KLOG("[DEBUG] sound0 ensure_ready fail err:%d state:%s\n",
			err, pcm_state_str(substream->runtime->status.state));
		// #endregion
		return err;
	}

	int ret = snd_pcm_write1(pcm, buf, size, offset);
	ALOG("write exit ret=%d state=%s appl=%d hw=%d avail_bytes=%d\n",
		ret, pcm_state_str(substream->runtime->status.state),
		substream->runtime->status.appl_ptr,
		substream->runtime->status.hw_ptr,
		snd_pcm_buf_avail(substream));
	// #region debug-point A:write-exit
	KLOG("[DEBUG] sound0 write exit ret:%d state:%s appl:%d hw:%d avail_bytes:%d\n",
		ret, pcm_state_str(substream->runtime->status.state),
		substream->runtime->status.appl_ptr,
		substream->runtime->status.hw_ptr,
		snd_pcm_buf_avail(substream));
	// #endregion
	return ret;
}

int fdev_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* buf, int size, int offset, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(offset);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	struct snd_pcm_substream *substream = pcm->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret = 0;

	snd_pcm_lock(substream);
	switch (runtime->status.state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		break;
	case PCM_STATE_XRUN:
		snd_pcm_unlock(substream);
		return -EPIPE;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	snd_pcm_unlock(substream);

	int avail = capture_avail(runtime);
	if (avail == 0) {
		return -EAGAIN;
	}

	int copy_frames = size > avail ? avail : size;
	ret = snd_pcm_substream_read(substream, buf, copy_frames);
	if (ret > 0) {
		ret = ret * runtime->frame_size;
	}

	return ret;
}

int fdev_ctrl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p)
{
	UNUSED(dev);
	UNUSED(from_pid);
	struct snd_pcm *pcm = (struct snd_pcm *)p;
	struct snd_pcm_substream *substream = pcm->substream;
	int result = 0;

	switch (cmd) {
	case CTRL_PCM_DEV_HW: {
		struct pcm_config config;
		memset(&config, 0, sizeof(struct pcm_config));
		proto_read_to(in, &config, sizeof(struct pcm_config));
		ALOG("ctrl hw bits=%d rate=%d ch=%d period=%d count=%d start=%d stop=%d state=%s\n",
			config.bit_depth, config.rate, config.channels,
			config.period_size, config.period_count,
			config.start_threshold, config.stop_threshold,
			pcm_state_str(substream->runtime->status.state));
		// #region debug-point B:ctrl-hw
		KLOG("[DEBUG] sound0 ctrl hw bits:%d rate:%d ch:%d period:%d count:%d start:%d stop:%d state:%s\n",
			config.bit_depth, config.rate, config.channels,
			config.period_size, config.period_count,
			config.start_threshold, config.stop_threshold,
			pcm_state_str(substream->runtime->status.state));
		// #endregion
		result = snd_pcm_hw_sw_parms(substream, &config);
		break;
	}
	case CTRL_PCM_DEV_HW_FREE:
		ALOG("ctrl hw_free state=%s appl=%d hw=%d\n",
			pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		// #region debug-point B:ctrl-hw-free
		KLOG("[DEBUG] sound0 ctrl hw_free state:%s\n",
			pcm_state_str(substream->runtime->status.state));
		// #endregion
		result = snd_pcm_hw_free(substream);
		break;
	case CTRL_PCM_DEV_PRPARE:
		ALOG("ctrl prepare state=%s appl=%d hw=%d\n",
			pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		// #region debug-point C:ctrl-prepare
		KLOG("[DEBUG] sound0 ctrl prepare state:%s\n",
			pcm_state_str(substream->runtime->status.state));
		// #endregion
		result = snd_pcm_prepare(substream);
		break;
	case CTRL_PCM_BUF_AVAIL:
		result = snd_pcm_ensure_query_ready(substream);
		if (result != 0) {
			ALOG("ctrl buf_avail query_fail ret=%d state=%s appl=%d hw=%d\n",
				result, pcm_state_str(substream->runtime->status.state),
				substream->runtime->status.appl_ptr,
				substream->runtime->status.hw_ptr);
			KLOG("[DEBUG] sound0 ctrl buf_avail query_fail ret:%d state:%s appl:%d hw:%d\n",
				result, pcm_state_str(substream->runtime->status.state),
				substream->runtime->status.appl_ptr,
				substream->runtime->status.hw_ptr);
			break;
		}
		/*
		 * User-space clients on miyoo treat CTRL_PCM_BUF_AVAIL as a
		 * "can I keep feeding pcm_write()?" gate. Returning the tiny
		 * instantaneous free space while playback is running makes those
		 * clients abort early even though the blocking write path can
		 * wait and complete the full transfer safely.
		 *
		 * Expose the full DMA buffer as the writable window here and let
		 * fdev_write()/snd_pcm_substeam_write() handle the actual pacing.
		 */
		result = substream->runtime->dma_bytes;
		ALOG("ctrl buf_avail ret=%d state=%s appl=%d hw=%d\n",
			result, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		// #region debug-point A:ctrl-buf-avail
		KLOG("[DEBUG] sound0 ctrl buf_avail ret:%d state:%s appl:%d hw:%d\n",
			result, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		// #endregion
		break;
	default:
		KLOG("fdev_ctrl() error! unkown cmd:%d\n", cmd);
		return -EINVAL;
	}

	PF->addi(ret, result);
	return 0;
}

static uint32_t fdev_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	struct snd_pcm_substream *substream = pcm->substream;
	int state = get_pcm_state(substream);

	if (substream->open_count == 0) {
		return 0;
	}
	if (state == PCM_STATE_UNKOWN || state == PCM_STATE_OPEN) {
		return 0;
	}
	int query_ret = snd_pcm_ensure_query_ready(substream);
	if (query_ret != 0) {
		ALOG("poll query_fail ret=%d state=%s appl=%d hw=%d\n",
			query_ret, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		KLOG("[DEBUG] sound0 poll query_fail ret:%d state:%s appl:%d hw:%d\n",
			query_ret, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		return 0;
	}
	int avail = snd_pcm_buf_avail(substream);
	if (avail > 0) {
		return VFS_EVT_WR;
	}
	if (avail < 0) {
		ALOG("poll avail_fail ret=%d state=%s appl=%d hw=%d\n",
			avail, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
		KLOG("[DEBUG] sound0 poll avail_fail ret:%d state:%s appl:%d hw:%d\n",
			avail, pcm_state_str(substream->runtime->status.state),
			substream->runtime->status.appl_ptr,
			substream->runtime->status.hw_ptr);
	}
	return 0;
}

static int fdev_loop_step(vdevice_t* dev, void* p)
{
	if (fdev_check_poll_events(dev, 0, 0, NULL, p) != 0) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
	}
	usleep(1000);
	return 0;
}

static struct file_operation vdev_ops = {
	.open = fdev_open,
	.close = fdev_close,
	.read = fdev_read,
	.write = fdev_write,
	.dev_cntl = fdev_ctrl,
};

static int snd_pcm_device_create(struct snd_device *device)
{
	struct snd_pcm *pcm = (struct snd_pcm *)device->owner;
	struct file_operation *fops = (struct file_operation *)device->param;
	vdevice_t *vdev = malloc(sizeof(vdevice_t));
	if (vdev == NULL) {
		return -ENOMEM;
	}

	memset(vdev, 0, sizeof(vdevice_t));

	strcpy(vdev->name, "sound");
	vdev->open = fops->open;
	vdev->close = fops->close;
	vdev->dev_cntl = fops->dev_cntl;
	vdev->read = fops->read;
	vdev->write = fops->write;
	vdev->check_poll_events = fdev_check_poll_events;
	vdev->loop_step = fdev_loop_step;

	vdev->extra_data = pcm;
	pcm->private_data = vdev;
	KLOG(">>>>>>%s() pcm-device register done!\n", __func__);
	return 0;
}

static int snd_pcm_device_free(struct snd_device *device)
{
	if (!device) {
		return 0;
	}

	struct snd_pcm *pcm = (struct snd_pcm *)device->owner;
	vdevice_t *vdev = pcm->private_data;
	if (vdev) {
		free(vdev);
	}
	return 0;
}
