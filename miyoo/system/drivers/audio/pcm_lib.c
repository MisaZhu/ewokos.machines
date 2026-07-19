#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <ewoksys/vfs.h>
#include <sys/errno.h>

#include "pcm_lib.h"

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
#define PCM_WAIT_AVAIL_SLEEP_US		5000
#define PCM_WAIT_AVAIL_MAX_SLEEP_US	40000
#define PCM_LOOP_IDLE_SLEEP_US		10000
#define PCM_LOOP_ACTIVE_SLEEP_US	20000
#define PCM_LOOP_CLOSED_SLEEP_US	160000
#define PCM_LOOP_IDLE_MAX_SLEEP_US	40000

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

	snd_card_unregister(card);

	struct listnode *node;
	struct listnode *node_next;
	list_for_each_safe(node, node_next, &card->pcm_list) {
		struct snd_pcm *pcm = node_to_item(node, struct snd_pcm, list);
		snd_pcm_free(pcm);
	}
	free(card);
	return 0;
}

int snd_card_register(struct snd_card *card)
{
	if (card == NULL) {
		return -EBADF;
	}

	struct listnode *node;
	list_for_each(node, &card->dev_list) {
		struct snd_device *cur_dev = node_to_item(node, struct snd_device, list);
		if (cur_dev->dev_new == NULL) {
			return -EINVAL;
		}
		int ret = cur_dev->dev_new(cur_dev);
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

int snd_card_unregister(struct snd_card *card)
{
	if (card == NULL) {
		return -EBADF;
	}

	struct listnode *node;
	list_for_each(node, &card->dev_list) {
		struct snd_device *cur_dev = node_to_item(node, struct snd_device, list);
		if (cur_dev->dev_free != NULL) {
			cur_dev->dev_free(cur_dev);
		}
	}
	return 0;
}

int snd_card_info_print(struct snd_card *card)
{
	if (card == NULL) {
		return 0;
	}
	
	
	
	
	if (card->num_pcm == 0) {
		
	} else {
		struct listnode *node;
		int index = 0;
		list_for_each(node, &card->pcm_list) {
			struct snd_pcm_substream *substream = NULL;
			struct snd_pcm *pcm = node_to_item(node, struct snd_pcm, list);
			substream = pcm->substream;
			
			
			if (substream != NULL) {
				
			}
		}
	}
	return 0;
}

static int snd_pcm_substream_new(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;

	substream = malloc(sizeof(*substream));
	if (substream == NULL) {
		return -ENOMEM;
	}
	memset(substream, 0, sizeof(*substream));
	pcm->substream = substream;
	substream->pcm = pcm;
	snprintf(substream->name, sizeof(substream->name), "substream-0");

	runtime = malloc(sizeof(*runtime));
	if (runtime == NULL) {
		free(substream);
		pcm->substream = NULL;
		return -ENOMEM;
	}

	if (pthread_mutex_init(&substream->lock, NULL) != 0) {
		free(runtime);
		free(substream);
		pcm->substream = NULL;
		return -ENOMEM;
	}
	memset(runtime, 0, sizeof(*runtime));
	substream->runtime = runtime;
	return 0;
}

static int snd_pcm_substream_free(struct snd_pcm *pcm)
{
	if (pcm->substream == NULL) {
		return 0;
	}

	pthread_mutex_destroy(&pcm->substream->lock);
	free(pcm->substream->runtime);
	pcm->substream->runtime = NULL;
	free(pcm->substream);
	pcm->substream = NULL;
	return 0;
}

int snd_pcm_lock(struct snd_pcm_substream *substream)
{
	if (substream == NULL) {
		return -EBADF;
	}
	pthread_mutex_lock(&substream->lock);
	return 0;
}

int snd_pcm_unlock(struct snd_pcm_substream *substream)
{
	if (substream == NULL) {
		return -EBADF;
	}
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
	pcm->dev = device;
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
	if (pcm->dev != NULL) {
		list_remove(&pcm->dev->list);
		free(pcm->dev);
		pcm->dev = NULL;
	}
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
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
	if (dai == NULL || dai->ops == NULL)
		return 0;
	if (dai->ops->pointer) {
		pos = dai->ops->pointer(dai, substream);
	} else {
		pos = 0;
	}
	return pos;
}

static int snd_dai_pcm_ack(struct snd_pcm_substream *substream)
{
	struct snd_pcm *pcm = substream->pcm;
	struct snd_soc_dai *dai = pcm->mem_dai;
	int ret = 0;
	if (dai == NULL || dai->ops == NULL)
		return 0;
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
		if (dai == NULL || dai->ops == NULL)
			continue;
		if (dai->ops->kick) {
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
	if (substream == NULL || substream->runtime == NULL || ravail == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int32_t avail = 0;
	int err = 0;

	while (1) {
		snd_pcm_lock(substream);

		/* Bail out immediately if the stream is being torn down. */
		if (substream->closing || substream->open_count == 0) {
			snd_pcm_unlock(substream);
			return -EBADF;
		}

		if (runtime->status.state == PCM_STATE_RUNNING && substream->ops->pointer) {
			update_hw_ptr(substream, 0);
		}
		avail = play_avail(runtime);
		if (avail >= runtime->period_size) {
			snd_pcm_unlock(substream);
			break;
		}

		//check pcm state again
		switch (runtime->status.state) {
		case PCM_STATE_XRUN: {
			err = -EPIPE;
			snd_pcm_unlock(substream);
			} return err;
		case PCM_STATE_OPEN:
		case PCM_STATE_SETUP: {
			err = -EBADF;
			snd_pcm_unlock(substream);
			} return err;
		case PCM_STATE_STOPED:
		case PCM_STATE_UNKOWN: {
			/*
			 * STOPED means the stream was explicitly halted (close or
			 * user trigger-stop). There is no producer that will advance
			 * hw_ptr, so waiting here would loop forever. Return an
			 * error and let the caller decide whether to re-prepare.
			 */
			err = -EBADF;
			snd_pcm_unlock(substream);
			} return err;
		default:
			snd_pcm_unlock(substream);
			break;
		}

		/*
		 * Adaptive sleep: estimate how long the hardware needs to
		 * consume (period_size - avail) frames at the current rate.
		 * This avoids waking every 5ms when a full period takes 21ms+
		 * (48kHz/1024), cutting write-thread wakeups by ~3-4x.
		 */
		{
			uint32_t sleep_us = PCM_WAIT_AVAIL_SLEEP_US;
			if (runtime->rate > 0) {
				int32_t need = runtime->period_size - avail;
				if (need > 0) {
					sleep_us = (uint32_t)(
						(uint64_t)need * 1000000ULL / runtime->rate);
					sleep_us += sleep_us >> 2; /* +25% margin */
				}
			}
			if (sleep_us < PCM_WAIT_AVAIL_SLEEP_US)
				sleep_us = PCM_WAIT_AVAIL_SLEEP_US;
			if (sleep_us > PCM_WAIT_AVAIL_MAX_SLEEP_US)
				sleep_us = PCM_WAIT_AVAIL_MAX_SLEEP_US;
			usleep(sleep_us);
		}
	}

	*ravail = avail;
	return err;
}

void snd_dump_substream(struct snd_pcm_substream *substream, int is_interrupt)
{
	if (substream == NULL || substream->runtime == NULL) {
		return;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	
}


int update_hw_ptr(struct snd_pcm_substream *substream, int is_interrupt)
{
	if (substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	if (substream->closing || substream->open_count == 0) {
		return 0;
	}
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
	if (substream->closing || substream->ops == NULL || substream->ops->pointer == NULL) {
		return 0;
	}
	if (substream->ops->pointer) {
		pos = substream->ops->pointer(substream);
	}

	if (pos >= runtime->buffer_size) {
		
		pos = 0;
	}


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
	if (delta < 0 && runtime->boundary > 0) {
		delta += runtime->boundary;
	}
	if (delta < 0) {
		delta = 0;
	}

	/*
	 * new_hw_ptr must always stay inside [hw_base, hw_base + buffer_size).
	 * If the pointer callback returns a stale pos that pushes us past
	 * boundary we wrap rather than write an out-of-range hw_ptr, which
	 * would otherwise feed a giant pos into the ring and smash dma_area.
	 */
	if (new_hw_ptr >= runtime->boundary) {
		new_hw_ptr -= runtime->boundary;
		hw_base = 0;
		cross_boundary++;
	}

	if (delta >= runtime->buffer_size) {
		
	}

	WRITE_LONG(&runtime->hw_ptr_base, hw_base);
	WRITE_LONG(&(runtime->status.hw_ptr), new_hw_ptr);

	runtime->irq_count++;

#if 0
	
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

		
	}

	return 0;
}

static int do_transfer(struct snd_pcm_runtime *runtime,
						int appl_off,
						const void *src_base,
						int off,
						int frames)
{
	volatile uint8_t *hw_app_ptr;
	const uint8_t *user_ptr;
	int bytes = 0;
	int dma_off = 0;

	if (runtime == NULL) {
		return -EBADF;
	}
	if (runtime->dma_area == NULL || runtime->dma_bytes <= 0) {
		return -EBADF;
	}
	if (src_base == NULL) {
		return -EBADF;
	}
	if (frames <= 0 || appl_off < 0 || appl_off >= runtime->buffer_size) {
		return -EINVAL;
	}
	if (appl_off + frames > runtime->buffer_size) {
		return -EINVAL;
	}
	if (off < 0) {
		return -EINVAL;
	}
	if (runtime->frame_size <= 0) {
		return -EBADF;
	}

	bytes = frame_to_bytes(runtime, frames);
	if (bytes <= 0) {
		return -EINVAL;
	}

	/*
	 * The DMA ring only has dma_bytes available. appl_off is in frames
	 * within buffer_size, but buffer_size can be larger than dma_bytes
	 * when HW only reserved a smaller region. Convert to a byte offset
	 * and clip the copy so a malformed config or a stale pointer cannot
	 * walk past the DMA ring.
	 */
	dma_off = frame_to_bytes(runtime, appl_off);
	if (dma_off < 0 || dma_off >= runtime->dma_bytes) {
		return -EINVAL;
	}
	if (bytes > runtime->dma_bytes - dma_off) {
		bytes = runtime->dma_bytes - dma_off;
	}
	if (bytes <= 0) {
		return -EINVAL;
	}

	hw_app_ptr = (volatile uint8_t *)runtime->dma_area + dma_off;
	user_ptr = (const uint8_t *)src_base + frame_to_bytes(runtime, off);

	memcpy((void *)hw_app_ptr, user_ptr, bytes);
	return 0;
}

int snd_pcm_substeam_write(struct snd_pcm_substream *substream, const void *source, int size)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int offset = 0;
	int avail = 0;
	int written = 0;
	int err = 0;

	if (substream == NULL || runtime == NULL) {
		return -EBADF;
	}
	if (source == NULL && size != 0) {
		return -EBADF;
	}
	if (runtime->buffer_size <= 0 || runtime->frame_size <= 0) {
		return -EBADF;
	}
	if (runtime->status.appl_ptr < 0) {
		WRITE_LONG(&(runtime->status.appl_ptr), 0);
	}

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
		update_hw_ptr(substream, 0);
	}
#endif
	snd_pcm_unlock(substream);
	runtime = substream->runtime;
	if (runtime == NULL) {
		return written > 0 ? written : -EBADF;
	}
	avail = play_avail(runtime);
	while(size > 0) {
		int copy_frames = 0;
		int to_end = 0;
		int appl_ptr = 0;
		int app_offset = 0;
		//int old_appl = 0;

		if (avail == 0) {
			err = wait_avail(substream, &avail);
			if (err < 0) {
				
				
				break;
			}
		}

		copy_frames = size > avail ? avail : size;
		if (copy_frames <= 0) {
			break;
		}
		snd_pcm_lock(substream);
		if (substream->open_count == 0 || substream->closing || substream->runtime == NULL ||
			substream->runtime->dma_area == NULL || substream->runtime->dma_bytes <= 0 ||
			substream->runtime->frame_size <= 0) {
			err = -EBADF;
			snd_pcm_unlock(substream);
			break;
		}
		runtime = substream->runtime;
		switch (runtime->status.state) {
		case PCM_STATE_PREPARE:
		case PCM_STATE_RUNNING:
			break;
		case PCM_STATE_XRUN:
			err = -EPIPE;
			snd_pcm_unlock(substream);
			break;
		default:
			err = -EBADF;
			snd_pcm_unlock(substream);
			break;
		}
		if (err < 0) {
			break;
		}
		if (runtime->status.appl_ptr < 0) {
			WRITE_LONG(&(runtime->status.appl_ptr), 0);
		}
		app_offset = runtime->status.appl_ptr % runtime->buffer_size;
		to_end = runtime->buffer_size - app_offset;
		if (to_end <= 0) {
			WRITE_LONG(&(runtime->status.appl_ptr), 0);
			snd_pcm_unlock(substream);
			continue;
		}
		if (copy_frames > to_end) {
			copy_frames = to_end;
		}

		err = do_transfer(runtime, app_offset, source, offset, copy_frames);
		if (err < 0) {
			snd_pcm_unlock(substream);
			break;
		}
		appl_ptr = runtime->status.appl_ptr + copy_frames;
		if (runtime->boundary > 0 && appl_ptr >= runtime->boundary) {
			appl_ptr -= runtime->boundary;
		}

		WRITE_LONG(&(runtime->status.appl_ptr), appl_ptr);

		if ((runtime->status.state == PCM_STATE_PREPARE) && (frames_ready(runtime) >= runtime->start_threshold)) {
			int temp = substream->ops->trigger(substream, PCM_TRIGER_START);
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
		snd_pcm_unlock(substream);

		offset += copy_frames;
		size -= copy_frames;
		written += copy_frames;
		if (avail > copy_frames) {
			avail -= copy_frames;
		} else {
			avail = 0;
		}
	}
	return (err < 0 ? err : written);
}

int snd_pcm_substream_read(struct snd_pcm_substream *substream, void *dest, int frames)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	if (dest == NULL || frames <= 0) {
		return -EINVAL;
	}
	if (runtime->buffer_size <= 0 || runtime->frame_size <= 0) {
		return -EBADF;
	}
	if (runtime->dma_area == NULL || runtime->dma_bytes <= 0) {
		return -EBADF;
	}
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
		int copy_bytes = 0;
		int hw_off_bytes = 0;
		int off_bytes = 0;
		char *hw_ptr_pos;
		char *user_ptr;

		if (avail == 0) {
			err = -EAGAIN;
			break;
		}

		copy_frames = frames > avail ? avail : frames;
		if (copy_frames <= 0) {
			break;
		}

		snd_pcm_lock(substream);
		if (substream->open_count == 0 || substream->closing ||
			runtime->dma_area == NULL || runtime->dma_bytes <= 0) {
			err = -EBADF;
			snd_pcm_unlock(substream);
			break;
		}
		switch (runtime->status.state) {
		case PCM_STATE_PREPARE:
		case PCM_STATE_RUNNING:
			break;
		case PCM_STATE_XRUN:
			err = -EPIPE;
			snd_pcm_unlock(substream);
			break;
		default:
			err = -EBADF;
			snd_pcm_unlock(substream);
			break;
		}
		if (err < 0) {
			break;
		}
		hw_offset = runtime->status.hw_ptr % runtime->buffer_size;
		to_end = runtime->buffer_size - hw_offset;
		if (to_end <= 0) {
			WRITE_LONG(&(runtime->status.hw_ptr), 0);
			snd_pcm_unlock(substream);
			continue;
		}
		if (copy_frames > to_end) {
			copy_frames = to_end;
		}
		copy_bytes = frame_to_bytes(runtime, copy_frames);
		if (copy_bytes <= 0) {
			snd_pcm_unlock(substream);
			break;
		}
		hw_off_bytes = frame_to_bytes(runtime, hw_offset);
		if (hw_off_bytes < 0 || hw_off_bytes >= runtime->dma_bytes) {
			snd_pcm_unlock(substream);
			break;
		}
		if (copy_bytes > runtime->dma_bytes - hw_off_bytes) {
			copy_bytes = runtime->dma_bytes - hw_off_bytes;
		}
		off_bytes = frame_to_bytes(runtime, offset);
		if (off_bytes < 0) {
			snd_pcm_unlock(substream);
			break;
		}
		hw_ptr_pos = runtime->dma_area + hw_off_bytes;
		user_ptr = (char *)dest + off_bytes;
		memcpy(user_ptr, hw_ptr_pos, copy_bytes);

		hw_ptr = runtime->status.hw_ptr + copy_frames;
		if (runtime->boundary > 0 && hw_ptr >= runtime->boundary) {
			hw_ptr -= runtime->boundary;
		}

		offset += copy_frames;
		frames -= copy_frames;
		read += copy_frames;
		avail -= copy_frames;

		WRITE_LONG(&(runtime->status.hw_ptr), hw_ptr);
		snd_pcm_unlock(substream);
	}

	return (err < 0 ? err : read);
}

int snd_pcm_buf_avail(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int avail_bytes = 0;

	snd_pcm_lock(substream);

	/*
	 * miyoo currently relies on a Timer0-driven polling path to advance hw_ptr.
	 * If that IRQ path stalls, userspace writes can block forever even when the
	 * hardware level register is still readable. Keep the state check and the
	 * fallback pointer poll under the same substream lock so close() cannot flip
	 * the stream to STOPED/OPEN and tear down the DAI bookkeeping in between.
	 */
	if (runtime->status.state == PCM_STATE_RUNNING && substream->ops->pointer) {
		update_hw_ptr(substream, 0);
	}

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

	snd_pcm_lock(substream);
	if (substream->open_count != 0) {
		snd_pcm_unlock(substream);
		return -EBUSY;
	}

	/*
	 * Reset the SW bookkeeping that a brand-new stream must start from
	 * a clean slate. Do NOT memset the whole runtime: hw_params() will
	 * refill dma_area/dma_bytes/dma_addr after us, and zeroing them
	 * here would turn the close->open window into a NULL-deref or a
	 * 0-length DMA on the very next write.
	 */
	runtime->status.appl_ptr = 0;
	runtime->status.hw_ptr = 0;
	runtime->hw_ptr_base = 0;
	runtime->ack_count = 0;
	runtime->irq_count = 0;
	runtime->time_pre = 0;
	runtime->time_after = 0;
	runtime->start_threshold = 0;
	runtime->stop_threshold = 0;
	runtime->boundary = 0;
	runtime->frame_bits = 0;
	runtime->frame_size = 0;
	runtime->bit_depth = 0;
	runtime->channels = 0;
	runtime->rate = 0;
	runtime->period_size = 0;
	runtime->periods = 0;
	runtime->buffer_size = 0;
	/*
	 * If the previous session never reached hw_free() (e.g. crashed
	 * userspace left hw_params set, or the device was force-closed
	 * from a different task), carry the stale DMA geometry over so
	 * hw_params() can refresh it; otherwise reset all three so a
	 * truly new session starts with no DMA mapping.
	 */
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;
	runtime->dma_area = NULL;

	runtime->status.state = PCM_STATE_OPEN;
	if (substream->ops != NULL && substream->ops->open != NULL) {
		err = substream->ops->open(substream);
	}

	if (err != 0) {
		set_pcm_state(substream, PCM_STATE_UNKOWN);
		snd_pcm_unlock(substream);
		return err;
	}
	substream->open_count = 1;
	snd_pcm_unlock(substream);
	return err;
}

static int snd_pcm_write1(struct snd_pcm *pcm,
		const void *data,
		int size,/* in bytes */
		int offset)
{
	if (pcm == NULL || pcm->substream == NULL) {
		return -EBADF;
	}
	struct snd_pcm_substream *substream  = pcm->substream;
	if (substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret = 0;
	int frame_size = 0;
	int state = PCM_STATE_UNKOWN;

	if (size == 0 || offset != 0) {
		return 0;
	}

	snd_pcm_lock(substream);
	if (substream->open_count == 0 ||
		!runtime->dma_area || runtime->dma_bytes <= 0 ||
		runtime->frame_size <= 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	frame_size = runtime->frame_size;
	state = runtime->status.state;
	snd_pcm_unlock(substream);

	if (state == PCM_STATE_PREPARE ||
		state == PCM_STATE_RUNNING) {
		ret = snd_pcm_substeam_write(substream, data, size / frame_size);
		if (ret > 0) {
			ret = ret * frame_size;
		}
	}
	return ret;
}


static void dump_pcm_runtime(struct snd_pcm_runtime *runtime)
{
	if (runtime != NULL) {
		
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
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	switch(runtime->status.state) {
	case PCM_STATE_OPEN:
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
		break;
	default:
		snd_pcm_unlock(substream);
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
	/*
	 * Guard against malformed configs (channels=0, bit_depth=0, bit_depth
	 * not a multiple of 8) silently producing frame_size=0, which would
	 * then divide-by-zero in snd_pcm_write1() / bytes_to_frames() and
	 * corrupt every offset calculation downstream.
	 */
	if (runtime->channels == 0 || runtime->bit_depth < 8 ||
		(runtime->bit_depth % 8) != 0) {
		
		set_pcm_state(substream, PCM_STATE_OPEN);
		snd_pcm_unlock(substream);
		return -EINVAL;
	}
	runtime->frame_size = runtime->channels * runtime->bit_depth / 8;
	runtime->boundary = runtime->buffer_size;
	if (runtime->boundary == 0) {
		
		snd_pcm_unlock(substream);
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
	if (runtime->buffer_size <= 0) {
		runtime->boundary = 1;
	} else {
		while (temp <= 0x3FFFFFFF && temp * 2 <= (uint32_t)(0x7FFFFFFF - runtime->buffer_size)) {
			temp *= 2;
		}
		runtime->boundary = (int32_t)temp;
	}

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
	snd_pcm_unlock(substream);
	
	return err;
}

static int snd_pcm_hw_free(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	switch(runtime->status.state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_STOPED:
		break;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}

	if (substream->ops->hw_free) {
		err = substream->ops->hw_free(substream);
	}

	set_pcm_state(substream, PCM_STATE_OPEN);
	snd_pcm_unlock(substream);
	return err;
}

static int snd_pcm_prepare(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = 0;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	switch(runtime->status.state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_STOPED:
	case PCM_STATE_XRUN:
		break;
	case PCM_STATE_OPEN:
		snd_pcm_unlock(substream);
		return -EBADF;
	case PCM_STATE_RUNNING:
		snd_pcm_unlock(substream);
		return -EBUSY;
	default:
		snd_pcm_unlock(substream);
		return -EBADF;
	}

	if (!substream->ops->prepare) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}

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
	snd_pcm_unlock(substream);
	return err;
}

static int snd_pcm_ensure_default_hw(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct pcm_config config;
	int state;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);

	if (state != PCM_STATE_OPEN) {
		return 0;
	}

	snd_pcm_default_config(&config);
	return snd_pcm_hw_sw_parms(substream, &config);
}

static int snd_pcm_ensure_write_ready(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	int err;
	int state;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);

	err = snd_pcm_ensure_default_hw(substream);
	if (err != 0) {
		return err;
	}

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);
	switch (state) {
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		return 0;
	case PCM_STATE_SETUP:
		return snd_pcm_prepare(substream);
	case PCM_STATE_XRUN:
		return -EPIPE;
	case PCM_STATE_STOPED:
		/*
		 * STOPED means the stream was explicitly halted. Auto-preparing
		 * here would reset hw_ptr/appl_ptr and restart DMA behind the
		 * back of any concurrent reader/poller, corrupting the session.
		 * Require an explicit user prepare (CTRL_PCM_DEV_PRPARE).
		 */
		return -EBADF;
	default:
		return -EBADF;
	}
}

static int snd_pcm_ensure_query_ready(struct snd_pcm_substream *substream)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	int err;
	int state;

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);

	err = snd_pcm_ensure_default_hw(substream);
	if (err != 0) {
		return err;
	}

	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return -EBADF;
	}
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);
	switch (state) {
	case PCM_STATE_SETUP:
	case PCM_STATE_PREPARE:
	case PCM_STATE_RUNNING:
		return 0;
	case PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADF;
	}
}

static int snd_pcm_release_substream(struct snd_pcm_substream *substream)
{
	int state;

	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;

	snd_pcm_lock(substream);
	state = get_pcm_state(substream);
	snd_pcm_unlock(substream);
	if (state == PCM_STATE_RUNNING ||
		state == PCM_STATE_PREPARE) {
		if (substream->ops->trigger) {
			substream->ops->trigger(substream, PCM_TRIGER_STOP);
		}

		snd_pcm_lock(substream);
		set_pcm_state(substream, PCM_STATE_STOPED);
		state = get_pcm_state(substream);
		snd_pcm_unlock(substream);
	}

	if (state == PCM_STATE_XRUN) {
		/*
		 * The IRQ handler may have already set processed_bytes = total_bytes
		 * and forced the trigger to STOP, but the engine bookkeeping in
		 * bach_runtime is still in a "running" state from the BACH side.
		 * Re-asserting STOP here, and clearing hw_ptr/appl_ptr, makes sure
		 * the next open()/wait_avail() doesn't see stale 'just-drained'
		 * values that were never re-initialized. Keep the resetting tight:
		 * only the fields that drive play_avail() and the XRUN judgement
		 * are touched, DMA geometry stays intact for hw_free().
		 */
		if (substream->ops->trigger) {
			substream->ops->trigger(substream, PCM_TRIGER_STOP);
		}
		snd_pcm_lock(substream);
		set_pcm_state(substream, PCM_STATE_STOPED);
		state = get_pcm_state(substream);
		WRITE_LONG(&(runtime->status.hw_ptr), 0);
		WRITE_LONG(&(runtime->hw_ptr_base), 0);
		runtime->status.appl_ptr = 0;
		runtime->ack_count = 0;
		runtime->irq_count = 0;
		snd_pcm_unlock(substream);
		
	}

	if (state == PCM_STATE_SETUP ||
		state == PCM_STATE_XRUN ||
		state ==  PCM_STATE_STOPED) {
		if (substream->ops->hw_free) {
			substream->ops->hw_free(substream);
		}

		snd_pcm_lock(substream);
		set_pcm_state(substream, PCM_STATE_OPEN);
		state = get_pcm_state(substream);
		snd_pcm_unlock(substream);
	}

	if (state == PCM_STATE_OPEN) {
		if (substream->ops->close) {
			substream->ops->close(substream);
		}
		snd_pcm_lock(substream);
		set_pcm_state(substream, PCM_STATE_UNKOWN);
		snd_pcm_unlock(substream);
	}

	return 0;
}

static int snd_pcm_release(struct snd_pcm *pcm)
{
	if (pcm == NULL || pcm->substream == NULL) {
		return -EBADF;
	}
	struct snd_pcm_substream *substream = pcm->substream;

	/*
	 * Signal all concurrent paths (write loop, pointer poll, ack) to
	 * bail out immediately.  closing=1 acts as a barrier: new operations
	 * check it and return early, so by the time we reach hw_free()/close()
	 * no thread is touching the DMA state.
	 */
	snd_pcm_lock(substream);
	substream->closing = 1;
	snd_pcm_unlock(substream);

	/* Stop DMA and tear down while closing=1 prevents new accesses */
	snd_pcm_release_substream(substream);

	snd_pcm_lock(substream);
	substream->open_count = 0;
	substream->closing = 0;
	snd_pcm_unlock(substream);

	return 0;
}


/***** File operations: Make vdevice conectted with PCM ***/
int fdev_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, int oflag, void* p)
{
	
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(oflag);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return -EBADF;
	}
	
	return snd_pcm_open(pcm);
}


int fdev_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* info, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(node);
	UNUSED(info);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return -EBADF;
	}
	int err = 0;
	
	err = snd_pcm_release(pcm);
	
	return err;
}


int fdev_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, const void* buf, int size, int offset, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_substream *substream = pcm->substream;
	int err = snd_pcm_ensure_write_ready(substream);
	if (err != 0) {
		
		return err;
	}

	return snd_pcm_write1(pcm, buf, size, offset);
}

int fdev_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* buf, int size, int offset, void* p)
{
	UNUSED(dev);
	UNUSED(fd);
	UNUSED(from_pid);
	UNUSED(info);
	UNUSED(offset);

	struct snd_pcm *pcm = (struct snd_pcm *)p;
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return -EBADF;
	}
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

	snd_pcm_lock(substream);
	int avail = capture_avail(runtime);
	snd_pcm_unlock(substream);
	if (avail == 0) {
		return -EAGAIN;
	}

	int copy_frames = size > avail ? avail : size;
	ret = snd_pcm_substream_read(substream, buf, copy_frames);
	if (ret > 0) {
		if (runtime->frame_size > 0) {
			ret = ret * runtime->frame_size;
		}
	}

	return ret;
}

int fdev_ctrl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p)
{
	UNUSED(dev);
	UNUSED(from_pid);
	struct snd_pcm *pcm = (struct snd_pcm *)p;
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_substream *substream = pcm->substream;
	int result = 0;

	switch (cmd) {
	case CTRL_PCM_DEV_HW: {
		struct pcm_config config;
		memset(&config, 0, sizeof(struct pcm_config));
		proto_read_to(in, &config, sizeof(struct pcm_config));
		
		result = snd_pcm_hw_sw_parms(substream, &config);
		break;
	}
	case CTRL_PCM_DEV_HW_FREE:
		
		result = snd_pcm_hw_free(substream);
		break;
	case CTRL_PCM_DEV_PRPARE:
		
		result = snd_pcm_prepare(substream);
		break;
	case CTRL_PCM_BUF_AVAIL:
		result = snd_pcm_ensure_query_ready(substream);
		if (result != 0) {
			
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
		 * Guard against hw_free() having reset dma_bytes back to 0; in
		 * that case fall back to buffer_size, which is at least sane.
		 */
		snd_pcm_lock(substream);
		if (substream->open_count == 0) {
			result = -EBADF;
		} else if (substream->runtime->dma_bytes > 0) {
			result = substream->runtime->dma_bytes;
		} else if (substream->runtime->buffer_size > 0) {
			result = frame_to_bytes(substream->runtime,
				substream->runtime->buffer_size);
			if (result <= 0) {
				result = 0;
			}
		} else {
			result = 0;
		}
		snd_pcm_unlock(substream);
		break;
	default:
		
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
	if (pcm == NULL || pcm->substream == NULL || pcm->substream->runtime == NULL) {
		return 0;
	}
	struct snd_pcm_substream *substream = pcm->substream;
	int state = PCM_STATE_UNKOWN;

	/*
	 * Take the substream lock while we read open_count/state/runtime
	 * pointers. fdev_close() walks the same fields and tears down
	 * runtime->private_data / dma_area; without this lock the polling
	 * thread can race with close() and dereference a freed bach_runtime
	 * or a NULL dma_area, which is what was crashing audctrl under
	 * load.
	 */
	snd_pcm_lock(substream);
	if (substream->open_count == 0) {
		snd_pcm_unlock(substream);
		return 0;
	}
	state = get_pcm_state(substream);
	if (state == PCM_STATE_UNKOWN || state == PCM_STATE_OPEN) {
		snd_pcm_unlock(substream);
		return 0;
	}
	/*
	 * Release the lock before calling ensure_query_ready / buf_avail:
	 * those take the lock themselves, and holding it across the call
	 * would deadlock if the same thread path re-entered.
	 */
	snd_pcm_unlock(substream);

	snd_pcm_lock(substream);
	if (substream->runtime == NULL || substream->open_count == 0 || substream->closing) {
		snd_pcm_unlock(substream);
		return 0;
	}
	snd_pcm_unlock(substream);

	int query_ret = snd_pcm_ensure_query_ready(substream);
	if (query_ret != 0) {
		return 0;
	}

	snd_pcm_lock(substream);
	if (substream->open_count == 0 || substream->closing || substream->runtime == NULL) {
		snd_pcm_unlock(substream);
		return 0;
	}
	snd_pcm_unlock(substream);

	int avail = snd_pcm_buf_avail(substream);
	if (avail > 0) {
		return VFS_EVT_WR;
	}
	return 0;
}

/*
 * State-aware sleep helpers for the polling loop.
 * pcm_loop_closed_sleep: long sleep for terminal/closed states (160ms).
 * pcm_loop_idle_backoff: exponential backoff for idle-but-open states
 *   (10ms -> 20ms -> 40ms). Pass reset=1 to reset to minimum.
 */
static uint32_t _pcm_loop_idle_sleep_us = PCM_LOOP_IDLE_SLEEP_US;

static void pcm_loop_closed_sleep(void)
{
	usleep(PCM_LOOP_CLOSED_SLEEP_US);
}

static void pcm_loop_idle_backoff(int reset)
{
	if (reset) {
		_pcm_loop_idle_sleep_us = PCM_LOOP_IDLE_SLEEP_US;
		return;
	}
	usleep(_pcm_loop_idle_sleep_us);
	if (_pcm_loop_idle_sleep_us < PCM_LOOP_IDLE_MAX_SLEEP_US) {
		_pcm_loop_idle_sleep_us *= 2;
		if (_pcm_loop_idle_sleep_us > PCM_LOOP_IDLE_MAX_SLEEP_US) {
			_pcm_loop_idle_sleep_us = PCM_LOOP_IDLE_MAX_SLEEP_US;
		}
	}
}

static int fdev_loop_step(vdevice_t* dev, void* p)
{
	if (dev == NULL || dev->mnt_info.node == 0) {
		return 0;
	}
	struct snd_pcm *pcm = (struct snd_pcm *)p;
	struct snd_pcm_substream *substream = (pcm != NULL) ? pcm->substream : NULL;
	struct snd_pcm_runtime *runtime;
	int state;

	if (substream == NULL) {
		pcm_loop_closed_sleep();
		return 0;
	}

	snd_pcm_lock(substream);
	if (substream->open_count == 0 || substream->runtime == NULL) {
		snd_pcm_unlock(substream);
		pcm_loop_closed_sleep();
		return 0;
	}
	runtime = substream->runtime;
	state = runtime->status.state;
	snd_pcm_unlock(substream);

	switch (state) {
	case PCM_STATE_RUNNING:
		{
			uint32_t events = fdev_check_poll_events(dev, 0, 0, NULL, p);
			if (events != 0) {
				vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
			}
		}
		pcm_loop_idle_backoff(1);
		usleep(PCM_LOOP_ACTIVE_SLEEP_US);
		break;
	case PCM_STATE_PREPARE:
	case PCM_STATE_SETUP:
		pcm_loop_idle_backoff(0);
		break;
	case PCM_STATE_XRUN:
	case PCM_STATE_STOPED:
	default:
		pcm_loop_closed_sleep();
		break;
	}
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

	snprintf(vdev->name, sizeof(vdev->name), "sound");
	vdev->open = fops->open;
	vdev->close = fops->close;
	vdev->dev_cntl = fops->dev_cntl;
	vdev->read = fops->read;
	vdev->write = fops->write;
	vdev->check_poll_events = fdev_check_poll_events;
	vdev->loop_step = fdev_loop_step;

	vdev->extra_data = pcm;
	pcm->private_data = vdev;
	
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
		pcm->private_data = NULL;
	}
	return 0;
}
