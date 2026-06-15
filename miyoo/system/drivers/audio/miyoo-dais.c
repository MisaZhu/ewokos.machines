#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include <ewoksys/timer.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/syscall.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/kernel_tic.h>

#include "pcm_lib.h"
#include "miyoo-dais.h"
#include "reg_ctrl.h"
#include "infinity_reg.h"

/*
 * Use an explicit software timer to poll BACH progress.
 * On miyoo the bare IRQ_TIMER0 hook alone is not enough to guarantee
 * periodic callbacks during audio playback.
 */
/* #define POLLING_DMA_WITH_SYS_TIMER0	1 */

typedef unsigned char   U8;
typedef signed char     S8;
typedef unsigned short  U16;
typedef short           S16;

#define UNUSED(v)	((void)v)
#define ENOMEM		(12)
#define	EINVAL		(22)
#define ETIMEDOUT	(116)
#define	EBADF		(9)
#define	EPIPE		(32)
#define	EBUSY		(16)

#define ARRAY_SIZE(A) ((sizeof(A)/sizeof(*A)))

#define WRITE_BYTE(_reg, _val)      (*((volatile U8*)(_reg)))  = (U8)(_val)
#define WRITE_WORD(_reg, _val)      (*((volatile U16*)(_reg))) = (U16)(_val)
#define WRITE_LONG(_reg, _val)      (*((volatile U32*)(_reg))) = (U32)(_val)
#define READ_BYTE(_reg)             (*(volatile U8*)(_reg))
#define READ_WORD(_reg)             (*(volatile U16*)(_reg))
#define READ_LONG(_reg)             (*(volatile U32*)(_reg))

#define TO_MIUSIZE(_x)			(_x >> 3)
#define FROM_MIUSIZE(_x) 		(_x << 3)

#define MSC313_BACH_DMA_SUB_CHANNEL_EN			0
#define MSC313_BACH_DMA_SUB_CHANNEL_ADDR		0x4
#define MSC313_BACH_DMA_SUB_CHANNEL_SIZE		0x8
#define MSC313_BACH_DMA_SUB_CHANNEL_TRIGGER		0xc
#define MSC313_BACH_DMA_SUB_CHANNEL_OVERRUNTHRESHOLD	0x10
#define MSC313_BACH_DMA_SUB_CHANNEL_UNDERRUNTHRESHOLD	0x14
#define MSC313_BACK_DMA_SUB_CHANNEL_LEVEL		0x18

#define MSC313_BACH_DMA_CHANNEL_CTRL0	0x0
#define MSC313_BACH_DMA_CHANNEL_CTRL8	0x20
#define MSC313_SUB_CHANNEL_READER	0
#define MSC313_SUB_CHANNEL_WRITER	1

#define MSC313_BACH_SR0_SEL		0x4
#define MSC313_BACH_DMA_TEST_CTRL7	0x1dc

/* Bank 1 */
#define REG_MUX0SEL	0xc
#define REG_SINEGEN	0x1d4
/* Bank 2 */
#define REG_DMA_INT	0x21c

/* Audio top */
#define REG_ATOP_OFFSET		0x1000
#define REG_ATOP_ANALOG_CTRL0	(REG_ATOP_OFFSET + 0)
#define REG_ATOP_ANALOG_CTRL1	(REG_ATOP_OFFSET + 0x4)
#define REG_ATOP_ANALOG_CTRL3	(REG_ATOP_OFFSET + 0xc)

#define PERIOD_BYTES_MIN 0x100
#define PRE_ALLOCATED_PCM_BUF_MAX_SIZE (64 * 1024)
#define MIYOO_MIU_PHY_BASE 0x20000000U

#define MSC313_BACH		0x002a0400
#define MSC313_BACH_TOP		0x00206800
#define MSC313_BACH_CLK		0x028400

#define DELAY_INTERVAL_MS	(5 * 1000)
#define US_TO_MS(US)		(US / 1000)

static const int msc313_bach_src_rates[] = {
	8000,
	11025,
	12000, /* unsupported by alsa? */
	16000,
	22050,
	24000, /* unsupported by alsa? */
	32000,
	44100,
	48000,
};

struct msc313_bach_dma_channel;

struct msc313_bach_dma_sub_channel {
	struct msc313_bach_dma_channel *dma_channel;

	struct reg_field *count;
	struct reg_field *trigger;
	struct reg_field *init;
	struct reg_field *en;
	struct reg_field *addr_hi, *addr_lo;
	struct reg_field *size;
	struct reg_field *trigger_level;
	struct reg_field *overrunthreshold;
	struct reg_field *underrunthreshold;
	struct reg_field *level;

	struct snd_pcm_substream *substream;
};

struct msc313_bach_dma_channel {
	/*
	 * Enabling the channel might cause an interrupt
	 * and bust everything, this lock must be taken
	 * when doing something that might result in an
	 * interrupt and when handling interrupts.
	 */
	//spinlock_t lock;

	struct reg_field *rst;
	struct reg_field *en;
	struct reg_field *live_count_en;
	struct reg_field *rd_int_clear;
	struct reg_field *rd_empty_int_en;
	struct reg_field *rd_overrun_int_en;
	struct reg_field *rd_underrun_int_en;

	struct reg_field *wr_underrun_flag;
	struct reg_field *wr_overrun_flag;
	struct reg_field *rd_underrun_flag;
	struct reg_field *rd_overrun_flag;
	struct reg_field *rd_empty_flag;
	struct reg_field *wr_full_flag;
	struct reg_field *wr_localbuf_full_flag;
	struct reg_field *rd_localbuf_empty_flag;

	struct reg_field *dma_rd_mono;
	struct reg_field *dma_wr_mono;
	struct reg_field *dma_rd_mono_copy;

	struct msc313_bach_dma_sub_channel reader_writer[2];
};

struct msc313_bach {
	unsigned int clk;
	unsigned int  audiotop;
	unsigned int bach;

	/* Digital controls */
	struct reg_field *src2_sel;
	struct reg_field *src1_sel;

	/* DMA */
	struct reg_field *dma_int_en;
	struct msc313_bach_dma_channel dma_channels[1];

	/* Analog controls? */
	struct reg_field *codec_sel;

	/* Pre-allocated DMA memory when probe */
	unsigned int dma_areas;
};

struct msc313_bach_substream_runtime {
	struct msc313_bach_dma_sub_channel *sub_channel;
	bool running;

	int last_appl_ptr;

	/* Filled by prepare */
	ssize_t period_bytes;
	ssize_t max_inflight;
	unsigned max_level;

	/* Updated by queuing */
	ssize_t pending_bytes;
	ssize_t total_bytes;

	/* Updated by irq */
	unsigned irqs;
	unsigned empties;
	unsigned underruns;
	ssize_t processed_bytes;
};


static inline void delay(int32_t count)
{
	while (count > 0) count--;
}

static inline unsigned int msc313_bach_dma_miu_addr(ewokos_addr_t vaddr)
{
	ewokos_addr_t phy = dma_phy_addr(0, vaddr);
	if (phy < MIYOO_MIU_PHY_BASE) {
		
		return (unsigned int)phy;
	}
	return (unsigned int)(phy - MIYOO_MIU_PHY_BASE);
}


static int msc313_bach_get_level(struct msc313_bach_dma_sub_channel *sub_channel)
{
	unsigned level;
	regmap_field_write(sub_channel->count, 1);
	/*
	 * The count pulse latches a fresh level into the read register
	 * asynchronously, then we discard the first read and use the
	 * second. The original code used delay(100) twice, which burned
	 * a few hundred ns of pure CPU spin per call; with the pointer
	 * being polled from fdev_loop_step and the user write path this
	 * added up to a measurable share of CPU on miyoo. A few cycles
	 * of spin is enough to flush the MMIO write, and the second read
	 * already takes care of the hardware-side settle time.
	 */
	delay(8);
	regmap_field_read(sub_channel->level, &level);
	delay(8);
	regmap_field_read(sub_channel->level, &level);
	regmap_field_write(sub_channel->count, 0);
	return level;
}

#if 0
static void msc313_bach_dump_dmactrl(struct msc313_bach *bach)
{
	unsigned ctrl0;
	int i;

	for(i = 0; i < 0x9; i++){
		regmap_read(bach->bach, 0x100 + (i * 4), &ctrl0);
		
	}
}
#endif

static int msc313_bach_pcm_ack(struct snd_soc_dai *dai,
			       struct snd_pcm_substream *substream);

/*
 * No-timer path. We previously drove the BACH DMA poll cadence from
 * timer_set/timer_remove, which forced audctrl to depend on the user
 * space timerd and the order in which it came up. That dependency is
 * gone now: the BACH engine is fed by the data path itself. Every
 * pcm_lib.c write/read that lands new bytes through mi_cpu_dai_kick
 * also pushes the BACH pending queue, which is exactly what the
 * soft-timer poll used to do. mi_cpu_dai_close() no longer needs to
 * cancel any timer.
 */


int mi_cpu_dai_open(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	if (dai == NULL || substream == NULL || substream->runtime == NULL) {
		return -EINVAL;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach *bach = dai->private_data;
	if (bach == NULL) {
		return -EINVAL;
	}
	struct msc313_bach_substream_runtime *bach_runtime =
		runtime->private_data;
	struct msc313_bach_dma_channel *dma_channel = &bach->dma_channels[0];

	/*
	 * Keep bach_runtime alive across close()->loop_step races. The
	 * polling path can still reach pointer()/queue bookkeeping after
	 * userspace starts tearing the stream down, and freeing the object
	 * in close() turns that into a UAF. Reuse the existing runtime on
	 * the next open and only allocate once on the first open.
	 */
	if (bach_runtime == NULL) {
		bach_runtime = calloc(1, sizeof(*bach_runtime));
		if (!bach_runtime) {
			return -ENOMEM;
		}
		runtime->private_data = bach_runtime;
	}
	else {
		memset(bach_runtime, 0, sizeof(*bach_runtime));
	}

	/*
	 * The previous session's close() left sub_channel = NULL and
	 * running = false, but every caller of this DAI (kick/ack/pointer/
	 * trigger/prepare) dereferences bach_runtime->sub_channel
	 * unconditionally. Re-attach it here so a second open() reaches a
	 * fully wired DAI, and keep the substream link published
	 * independently so a parallel close() from a stale task cannot
	 * take the channel away mid-open.
	 */
	bach_runtime->sub_channel = &bach->dma_channels[0].reader_writer[0];
	bach_runtime->sub_channel->substream = substream;
	bach->dma_channels[0].reader_writer[0].substream = substream;

	/*
	 * Force the BACH channel into a known-quiescent state BEFORE the
	 * caller starts writing. The reset pulse alone is not enough: the
	 * sub_channel still carries the live count latch and the previous
	 * session's running flag, both of which would let queue_pending()
	 * promote stale pending_bytes on the first kick().
	 */
	regmap_field_write(dma_channel->rst, 1);
	regmap_field_write(dma_channel->en, 0);
	regmap_field_write(dma_channel->rd_empty_int_en, 0);
	regmap_field_write(dma_channel->rd_underrun_int_en, 0);
	delay(1000);
	regmap_field_write(dma_channel->rst, 0);
	delay(1000);

	/* Setup default register config */
	regmap_field_write(dma_channel->live_count_en, 1);

	return 0;
}

/*
 * Writer-side kick: called by pcm_lib.c right after ack() on every copy
 * chunk.  This guarantees that the bytes the user just produced are
 * handed off to BACH synchronously, so the next IRQ empty tail won't
 * observe pending_bytes == 0 and leave the DMA engine stranded.
 */
static void msc313_bach_queue_pending(struct msc313_bach *bach,
				     struct snd_pcm_substream *substream,
				     struct msc313_bach_substream_runtime *bach_runtime);

static int mi_cpu_dai_kick(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	UNUSED(dai);
	if (substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach_runtime == NULL) {
		return 0;
	}
	msc313_bach_queue_pending((struct msc313_bach *)dai->private_data, substream, bach_runtime);
	return 0;
}


int mi_cpu_dai_hw_params(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = dai->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int dma_phy_addr_hw;

	/*
	 * Keep the preallocated DMA area, but always refresh dma_bytes/dma_addr
	 * from the latest PCM params. Userspace may call hw_params more than once
	 * and returning early here leaves size registers stale.
	 */
	if (bach == NULL || runtime == NULL) {
		return -EINVAL;
	}
	if (runtime->buffer_size <= 0 || runtime->frame_size <= 0) {
		
		return -EINVAL;
	}
	if (runtime->dma_area == NULL) {
		if (bach->dma_areas == 0) {
			
			return -EINVAL;
		}
		runtime->dma_area = (char *)bach->dma_areas;
	}
	dma_phy_addr_hw = msc313_bach_dma_miu_addr((ewokos_addr_t)runtime->dma_area);
	runtime->dma_addr = dma_phy_addr_hw;
	runtime->dma_bytes = frame_to_bytes(runtime, runtime->buffer_size); //TODO
	if (runtime->dma_bytes <= 0) {
		
		return -EINVAL;
	}
	if ((uint32_t)runtime->dma_bytes > PRE_ALLOCATED_PCM_BUF_MAX_SIZE) {
		
		return -EINVAL;
	}
	/* clear allocated buffer */
	memset(runtime->dma_area, 0, runtime->dma_bytes);

	
	
	return 0;
}

int mi_cpu_dai_hw_free(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	UNUSED(dai);
	struct snd_pcm_runtime *runtime;

	if (substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	runtime = substream->runtime;
	if (runtime->dma_area == NULL) {
		return 0;
	}

	runtime->dma_addr = 0;
	/*
	 * The DMA area is owned by bach->dma_areas (pre-allocated once at
	 * driver init time). Reset dma_bytes/dma_area here so any later
	 * stale write through pcm_lib goes into the "no dma area" branch
	 * and is rejected with -EBADF instead of touching the freed/cached
	 * region.
	 */
	runtime->dma_bytes = 0;
	runtime->dma_area = NULL;
	
	return 0;
}

int mi_cpu_dai_close(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	if (dai == NULL || substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	struct msc313_bach *bach = dai->private_data;
	if (bach == NULL) {
		return 0;
	}
	struct msc313_bach_dma_channel *dma_channel = &bach->dma_channels[0];
	struct snd_pcm_runtime *runtime = substream->runtime;

	/*
	 * Stop asynchronous readers (timer/IRQ) FIRST and clear the global
	 * pointers under the substream lock, so any in-flight callback observes
	 * a consistent "nothing to do" state.
	 */
	snd_pcm_lock(substream);
	bach->dma_channels[0].reader_writer[0].substream = NULL;
	if (runtime->private_data != NULL) {
		struct msc313_bach_substream_runtime *bach_runtime =
			runtime->private_data;
		/*
		 * Do NOT free here: fdev_loop_step -> snd_pcm_buf_avail ->
		 * update_hw_ptr -> pointer() may still be in flight without
		 * holding substream->lock. Clearing the fields makes any late
		 * poll observe an inert runtime, and the object is reused or
		 * reinitialized in the next open().
		 */
		bach_runtime->running = false;
		bach_runtime->sub_channel = NULL;
		bach_runtime->pending_bytes = 0;
		bach_runtime->total_bytes = 0;
		bach_runtime->processed_bytes = 0;
		bach_runtime->last_appl_ptr = 0;
		bach_runtime->irqs = 0;
		bach_runtime->empties = 0;
		bach_runtime->underruns = 0;
		bach_runtime->period_bytes = 0;
		bach_runtime->max_inflight = 0;
		bach_runtime->max_level = 0;
	}
	snd_pcm_unlock(substream);

	/*
	 * Disable the BACH side under the same lock-then-reset discipline
	 * so the next open() doesn't observe a still-running engine. The
	 * substream is already unpublished above, but the DMA channel
	 * enable bits and underrun/empty IRQs would otherwise stay armed
	 * across the close->open window.
	 */
	regmap_field_write(dma_channel->en, 0);
	regmap_field_write(dma_channel->rd_empty_int_en, 0);
	regmap_field_write(dma_channel->rd_underrun_int_en, 0);
	regmap_field_write(dma_channel->rst, 1);


	return 0;
}

static int msc313_bach_queue_bytes(/*struct msc313_bach *bach,*/
				   struct snd_pcm_substream *substream,
				   ssize_t new_bytes)
{
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach_runtime == NULL || bach_runtime->sub_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_sub_channel *sub_channel = bach_runtime->sub_channel;
	if (sub_channel->dma_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_channel *dma_channel = sub_channel->dma_channel;
	unsigned trigbit;
	unsigned miu_trigger_level = TO_MIUSIZE(new_bytes);
	int target_level, old_level;

	old_level = msc313_bach_get_level(sub_channel);

	target_level = old_level + miu_trigger_level;
	if (target_level > (int)bach_runtime->max_level) {
		
		return -EINVAL;
	}

	regmap_field_write(sub_channel->trigger_level, miu_trigger_level);
	regmap_field_read(sub_channel->trigger, &trigbit);
	regmap_field_write(sub_channel->trigger, ~trigbit);

	bach_runtime->total_bytes += new_bytes;
	/*
	 * pending_bytes tracks "the user-ack'd amount that hasn't been pushed
	 * into BACH yet". msc313_bach_queue_pending() may have promoted a
	 * fallback value from play_avail() even when the real pending was 0,
	 * so clamp the subtraction at 0 here; otherwise a target_level cap
	 * (or a smaller-than-expected queue) would leave pending_bytes
	 * negative and corrupt the next iteration's accounting.
	 */
	if ((ssize_t)bach_runtime->pending_bytes > (ssize_t)new_bytes) {
		bach_runtime->pending_bytes -= new_bytes;
	} else {
		bach_runtime->pending_bytes = 0;
	}

	/* frequently CPU cost may make underrun */
	//delay(10 * 1000);
	//delay_level = msc313_bach_get_level(sub_channel);

#if 0
	if (delay_level == new_level)
		
#endif

	/* should be safe to turn the underrun and empty irq back on */
	regmap_field_write(dma_channel->rd_underrun_int_en, 1);
	regmap_field_write(dma_channel->rd_empty_int_en, 1);

#if 0
	//msc313_bach_pcm_dumpruntime(bach_runtime);
	
#endif
	runtime->ack_count = 0;
	return 0;
}

static void msc313_bach_queue_pending(struct msc313_bach *bach,
				     struct snd_pcm_substream *substream,
				     struct msc313_bach_substream_runtime *bach_runtime) {
	UNUSED(bach);
	if (bach_runtime == NULL || substream == NULL) {
		return;
	}
	ssize_t new_queue_bytes = 0;
	ssize_t hw_cache_bytes = 0;
	ssize_t pending_bytes = 0;
	ssize_t period_bytes = bach_runtime->period_bytes;
	if (period_bytes <= 0) {
		/*
		 * queue_bytes() floors its target on a whole period, so a
		 * zero/negative period would divide-by-zero in the
		 * (new_queue_bytes / period_bytes) alignment step below.
		 * Bail out instead of taking the SIGFPE path.
		 */
		return;
	}
	/*
	 * Trying to queue before the channel is running results in either
	 * the data not being queued or the dma locking up, so don't do that.
	 */
	if (!bach_runtime->running) {
		return;
	}

	/*
	 * pending_bytes is the single source of truth for "bytes userspace
	 * has produced but BACH has not yet been told about".  The IRQ tail
	 * path used to fall back to play_avail() when pending was 0, but
	 * that promoted too much, made the target_level check fail, and
	 * corrupted the next iteration's accounting.  If pending_bytes is
	 * empty here, the call will simply not refill.
	 */
	pending_bytes = bach_runtime->pending_bytes;
	if (pending_bytes <= 0) {
		return;
	}

	hw_cache_bytes = bach_runtime->total_bytes - bach_runtime->processed_bytes;
	/*
	 * If the hardware is already at or past the watermark, don't push
	 * more data; BACH will fire another empty/underrun IRQ when it has
	 * actually drained.
	 */
	if (hw_cache_bytes >= (ssize_t)bach_runtime->max_inflight) {
		return;
	}

	if (pending_bytes < period_bytes) {
		return;
	}

	if (hw_cache_bytes < 0) {
		
	}
	else if (hw_cache_bytes < (int)bach_runtime->max_inflight) {
		/*
		 * Keep BACH close to a stable inflight watermark instead of only
		 * topping up one period at a time. The 2-period cap was leaving too
		 * little runway on miyoo: userspace still had buffered audio, but the
		 * hardware queue could drain to XRUN before the next refill landed.
		 */
		new_queue_bytes = bach_runtime->max_inflight - hw_cache_bytes;
		if (pending_bytes < new_queue_bytes) {
			new_queue_bytes = pending_bytes;
		}
		if (new_queue_bytes > (int)period_bytes) {
			new_queue_bytes =
				(new_queue_bytes / period_bytes) *
				period_bytes;
		}
	} else {
		new_queue_bytes = 0;
	}

	if (new_queue_bytes >= (int)period_bytes) {
		msc313_bach_queue_bytes(substream, new_queue_bytes);
	}
}

int mi_cpu_dai_prepare(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	if (dai == NULL || substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct msc313_bach *bach = dai->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach == NULL || bach_runtime == NULL || bach_runtime->sub_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_sub_channel *sub_channel = bach_runtime->sub_channel;
	if (sub_channel->dma_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_channel *dma_channel = sub_channel->dma_channel;
	unsigned stride;
	unsigned miu_underrun_size, miu_buffer_size, miu_addr;
	unsigned mono = runtime->channels == 1 ? 1 : 0;
	unsigned int i;
	int ret;

	if (runtime->frame_bits < 8) {
		
		return -EINVAL;
	}
	stride = runtime->frame_bits / 8;

	bach_runtime->last_appl_ptr = 0;
	bach_runtime->running = false;
	bach_runtime->irqs = 0;
	bach_runtime->empties = 0;
	bach_runtime->underruns = 0;
	bach_runtime->pending_bytes = 0;
	bach_runtime->processed_bytes = 0;
	bach_runtime->total_bytes = 0;
	bach_runtime->period_bytes = frame_to_bytes(runtime, runtime->period_size);
	if (bach_runtime->period_bytes <= 0) {
		bach_runtime->period_bytes = runtime->dma_bytes > 0 ? runtime->dma_bytes : 0;
	}
	if (runtime->dma_bytes > 0 && runtime->dma_bytes >= bach_runtime->period_bytes) {
		bach_runtime->max_inflight = runtime->dma_bytes - bach_runtime->period_bytes;
	} else {
		bach_runtime->max_inflight = bach_runtime->period_bytes;
	}
	if (bach_runtime->max_inflight < bach_runtime->period_bytes * 2) {
		bach_runtime->max_inflight = bach_runtime->period_bytes * 2;
	}
	if (bach_runtime->max_inflight > runtime->dma_bytes) {
		bach_runtime->max_inflight = runtime->dma_bytes;
	}
	if (bach_runtime->max_inflight <= 0) {
		bach_runtime->max_inflight = bach_runtime->period_bytes;
	}

	miu_underrun_size = TO_MIUSIZE((bach_runtime->period_bytes + stride));
	miu_buffer_size = TO_MIUSIZE(runtime->dma_bytes);
	miu_addr = TO_MIUSIZE(runtime->dma_addr); //TODO
	bach_runtime->max_level = TO_MIUSIZE(bach_runtime->max_inflight);

	/* This is needed to reset the buffer level */
	regmap_field_write(sub_channel->trigger, 0);
	regmap_field_write(sub_channel->init, 1);
	regmap_field_write(sub_channel->init, 0);

	

	regmap_field_write(sub_channel->addr_hi, miu_addr >> 12);
	regmap_field_write(sub_channel->addr_lo, miu_addr);
	regmap_field_write(sub_channel->size, miu_buffer_size);

	/* We want an interrupt underrun when we hit the last frame of the second period */
	regmap_field_write(sub_channel->underrunthreshold, miu_underrun_size);
	/* This shouldn't really matter,.. */
	regmap_field_write(sub_channel->overrunthreshold, 0);

	//
	regmap_field_write(dma_channel->dma_rd_mono, mono);
	regmap_field_write(dma_channel->dma_rd_mono_copy, mono);

	ret = -EINVAL;
	for (i = 0; i < ARRAY_SIZE(msc313_bach_src_rates); i++) {
		if (msc313_bach_src_rates[i] == (int)substream->runtime->rate) {
			regmap_field_write(bach->src1_sel, i);
			ret = 0;
			break;
		}
	}

	
	
	return ret;
}

int mi_cpu_dai_trigger(struct snd_soc_dai *dai, struct snd_pcm_substream *substream, int cmd)
{
	UNUSED(dai);
	if (substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach_runtime == NULL || bach_runtime->sub_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_sub_channel *sub_channel = bach_runtime->sub_channel;
	if (sub_channel->dma_channel == NULL) {
		return -EBADF;
	}
	struct msc313_bach_dma_channel *dma_channel = sub_channel->dma_channel;
	unsigned int addr_hi = 0, addr_lo = 0, size = 0, underrun = 0;

	

	switch (cmd) {
	case PCM_TRIGER_START:
		/*
		 * Enabling the channel can cause interrupts before we are ready,
		 * take the lock to force an irq to wait until we are finished.
		 */

		/* Clear any pending interrupts */
		regmap_field_write(dma_channel->rd_int_clear, 1);
		regmap_field_write(dma_channel->rd_int_clear, 0);

		/* Unmask interrupts */
		regmap_field_write(dma_channel->rd_overrun_int_en, 0);
		//regmap_field_write(dma_channel->rd_underrun_int_en, 1); //TODO
		//regmap_field_write(dma_channel->rd_empty_int_en, 1); //1

		/*
		 * Note: it seems like enabling the DMA channel must happen right
		 * before enabling the reader or the reader locks up.
		 */
		regmap_field_write(dma_channel->en, 1);
		delay(1000);
		
		

		/* Start playback */
		regmap_field_write(sub_channel->en, 1);
		delay(1000);
		regmap_field_read(sub_channel->addr_hi, &addr_hi);
		regmap_field_read(sub_channel->addr_lo, &addr_lo);
		regmap_field_read(sub_channel->size, &size);
		regmap_field_read(sub_channel->underrunthreshold, &underrun);
		
		
		bach_runtime->running = true;
		//msc313_bach_queue_pending(bach, substream, bach_runtime);

		break;
	case PCM_TRIGER_STOP:
		regmap_field_write(sub_channel->en, 0);
		delay(1000);
		regmap_field_write(dma_channel->en, 0);
		/* Mask interrupts */
		regmap_field_write(dma_channel->rd_underrun_int_en, 0);
		regmap_field_write(dma_channel->rd_empty_int_en, 0);
		bach_runtime->running = false;
		break;
	default:
		
	}

	return 0;
}

int mi_cpu_dai_pointer(struct snd_soc_dai *dai, struct snd_pcm_substream *substream)
{
	UNUSED(dai);
	if (substream == NULL || substream->runtime == NULL) {
		return 0;
	}
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach_runtime == NULL || bach_runtime->sub_channel == NULL) {
		return 0;
	}
	struct msc313_bach_dma_sub_channel *sub_channel = bach_runtime->sub_channel;
	int pos;
	int level;
	ssize_t bytes_in_hw;
	ssize_t old_processed;
	ssize_t new_processed;

	/*
	 * The amount of bytes the channel is currently munching through is the difference
	 * between the bytes queued and the number of bytes that have been processed
	 * according to an IRQ coming.
	 */
	//inflight = bach_runtime->total_bytes - bach_runtime->processed_bytes;
	/*
	 * The number of bytes that have been processed before the next IRQ comes will
	 * be roughly the number of bytes that are waiting to be confirmed by an IRQ
	 * minus current number of bytes the hardware says it still hasn't processed.
	 */
	level = msc313_bach_get_level(sub_channel);
	bytes_in_hw = FROM_MIUSIZE(level);
	if (bytes_in_hw < 0) {
		bytes_in_hw = 0;
	}
	if (bytes_in_hw > bach_runtime->total_bytes) {
		bytes_in_hw = bach_runtime->total_bytes;
	}

	old_processed = bach_runtime->processed_bytes;
	new_processed = bach_runtime->total_bytes - bytes_in_hw;
	if (new_processed < 0) {
		new_processed = 0;
	}
	/*
	 * Keep pointer accounting monotonic across timer polls. The hardware level
	 * can briefly jump when userspace queues more data; letting processed_bytes
	 * move backwards here makes hw_ptr regress and can stall wait_avail/write.
	 */
	if (new_processed < old_processed) {
		new_processed = old_processed;
	}

	bach_runtime->processed_bytes = new_processed;
	if (runtime->dma_bytes > 0 && runtime->frame_size > 0) {
		ssize_t mod = new_processed % (ssize_t)runtime->dma_bytes;
		if (mod < 0) {
			mod += (ssize_t)runtime->dma_bytes;
		}
		pos = bytes_to_frames(runtime, (int)mod);
	} else {
		pos = 0;
	}

	return pos;
}

static int msc313_bach_pcm_ack(struct snd_soc_dai *dai,
			       struct snd_pcm_substream *substream)
{
	if (dai == NULL || substream == NULL || substream->runtime == NULL) {
		return -EBADF;
	}
	struct msc313_bach *bach = dai->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_substream_runtime *bach_runtime = runtime->private_data;
	if (bach == NULL || bach_runtime == NULL) {
		return -EBADF;
	}
	int new_bytes = 0;

	new_bytes = frame_to_bytes(runtime, runtime->status.appl_ptr - bach_runtime->last_appl_ptr);
	if (new_bytes < 0) {
		/*
		 * pending_bytes advances monotonically. appl_ptr must only
		 * move forward, but update_hw_ptr()/pointer() could in theory
		 * regress; in that case treat the delta as one full period of
		 * refill rather than a full buffer of data, otherwise a single
		 * regression can over-credit pending_bytes by dma_bytes and
		 * make the next queue step see a wildly inflated target level.
		 */
		int period_frames = (runtime->period_size > 0) ? runtime->period_size : 0;
		int period_byte = frame_to_bytes(runtime, period_frames);
		if (period_byte > 0) {
			new_bytes += period_byte;
		} else {
			new_bytes = 0;
		}
	}
	bach_runtime->last_appl_ptr = runtime->status.appl_ptr;
	bach_runtime->pending_bytes += new_bytes;

	if (bach_runtime->pending_bytes >= frame_to_bytes(runtime, runtime->period_size)) {
		runtime->ack_count = 1;
		msc313_bach_queue_pending(bach, substream, bach_runtime);
	}

	return 0;
}

static struct snd_soc_dai_ops msc313_cpu_dai_ops = {
	.open = mi_cpu_dai_open,
	.close = mi_cpu_dai_close,
	.hw_params = mi_cpu_dai_hw_params,
	.hw_free = mi_cpu_dai_hw_free,
	.prepare = mi_cpu_dai_prepare,
	.trigger = mi_cpu_dai_trigger,
	.pointer = mi_cpu_dai_pointer,
	.ack =  msc313_bach_pcm_ack,
	.kick = mi_cpu_dai_kick,
};

static void msc313_bach_the_horror(struct msc313_bach *bach)
{
	unsigned int ret_val = 0;
	regmap_write(bach->audiotop, 0x00, 0x00000A14);
	regmap_write(bach->audiotop, 0x04, 0x00000030);
	regmap_write(bach->audiotop, 0x08, 0x00000080);
	// power downs?
	regmap_write(bach->audiotop, 0x0C, 0x000001A5);
	// dac/adc resets
	regmap_write(bach->audiotop, 0x10, 0x00000000);
	regmap_write(bach->audiotop, 0x14, 0x00000000);
	regmap_write(bach->audiotop, 0x18, 0x00000000);
	regmap_write(bach->audiotop, 0x1C, 0x00000000);
	regmap_write(bach->audiotop, 0x20, 0x00003000);
	regmap_write(bach->audiotop, 0x24, 0x00000000);
	regmap_write(bach->audiotop, 0x28, 0x00000000);
	regmap_write(bach->audiotop, 0x2C, 0x00000000);
	regmap_write(bach->audiotop, 0x30, 0x00000000);
	regmap_write(bach->audiotop, 0x34, 0x00000000);
	regmap_write(bach->audiotop, 0x38, 0x00000000);
	regmap_write(bach->audiotop, 0x3C, 0x00000000);
	regmap_write(bach->audiotop, 0x40, 0x00000000);
	regmap_write(bach->audiotop, 0x44, 0x00000000);
	regmap_write(bach->audiotop, 0x48, 0x00000000);
	regmap_write(bach->audiotop, 0x4C, 0x00000000);
	regmap_write(bach->audiotop, 0x50, 0x00000000);
	regmap_write(bach->audiotop, 0x54, 0x00000000);
	regmap_write(bach->audiotop, 0x58, 0x00000000);
	regmap_write(bach->audiotop, 0x5C, 0x00000000);
	regmap_write(bach->audiotop, 0x60, 0x00000000);
	regmap_write(bach->audiotop, 0x64, 0x00000000);
	regmap_write(bach->audiotop, 0x68, 0x00000000);
	regmap_write(bach->audiotop, 0x6C, 0x00000000);
	regmap_write(bach->audiotop, 0x70, 0x00000000);
	regmap_write(bach->audiotop, 0x74, 0x00000000);
	regmap_write(bach->audiotop, 0x78, 0x00000000);
	regmap_write(bach->audiotop, 0x7C, 0x00000000);
	regmap_write(bach->audiotop, 0x80, 0x00000000);
	regmap_write(bach->audiotop, 0x84, 0x00003C1E);
	regmap_write(bach->audiotop, 0x88, 0x00000000);
	regmap_write(bach->audiotop, 0x8C, 0x00000000);
	regmap_write(bach->audiotop, 0x90, 0x00000000);
	regmap_write(bach->audiotop, 0x94, 0x00000000);
	regmap_write(bach->audiotop, 0x98, 0x00000000);
	regmap_write(bach->audiotop, 0x9C, 0x00000000);
	regmap_write(bach->audiotop, 0xA0, 0x00000000);
	regmap_write(bach->audiotop, 0xA4, 0x00000000);
	regmap_write(bach->audiotop, 0xA8, 0x00000000);
	regmap_write(bach->audiotop, 0xAC, 0x00000000);
	regmap_write(bach->audiotop, 0xB0, 0x00000000);
	regmap_write(bach->audiotop, 0xB4, 0x00000000);
	regmap_write(bach->audiotop, 0xB8, 0x00000000);
	regmap_write(bach->audiotop, 0xBC, 0x00000000);
	regmap_write(bach->audiotop, 0xC0, 0x00000000);
	regmap_write(bach->audiotop, 0xC4, 0x00000000);
	regmap_write(bach->audiotop, 0xC8, 0x00000000);
	regmap_write(bach->audiotop, 0xCC, 0x00000000);
	regmap_write(bach->audiotop, 0xD0, 0x00000000);
	regmap_write(bach->audiotop, 0xD4, 0x00000000);
	regmap_write(bach->audiotop, 0xD8, 0x00000000);
	regmap_write(bach->audiotop, 0xDC, 0x00000000);
	regmap_write(bach->audiotop, 0xE0, 0x00000000);
	regmap_write(bach->audiotop, 0xE4, 0x00000000);
	regmap_write(bach->audiotop, 0xE8, 0x00000000);
	regmap_write(bach->audiotop, 0xEC, 0x00000000);
	regmap_write(bach->audiotop, 0xF0, 0x00000000);
	regmap_write(bach->audiotop, 0xF4, 0x00000000);
	regmap_write(bach->audiotop, 0xF8, 0x00000000);
	regmap_write(bach->audiotop, 0xFC, 0x00000000);

	regmap_write(bach->bach, 0x00, 0x000089FF);
	regmap_write(bach->bach, 0x04, 0x0000FF88);
	regmap_write(bach->bach, 0x08, 0x00000003);
	regmap_write(bach->bach, 0x0C, 0x000019B4);
	regmap_write(bach->bach, 0x10, 0x0000F000);
	regmap_write(bach->bach, 0x14, 0x00008000);
	regmap_write(bach->bach, 0x18, 0x0000C09A);
	// MIX config?
	regmap_write(bach->bach, 0x1C, 0x0000555A);
	regmap_write(bach->bach, 0x20, 0x00000000);
	regmap_write(bach->bach, 0x24, 0x00000209);
	regmap_write(bach->bach, 0x28, 0x00000000);
	regmap_write(bach->bach, 0x2C, 0x0000007D);
	regmap_write(bach->bach, 0x30, 0x00000000);
	regmap_write(bach->bach, 0x34, 0x00000000);
	regmap_write(bach->bach, 0x38, 0x00003017);
	regmap_write(bach->bach, 0x3C, 0x00000002);
	regmap_write(bach->bach, 0x40, 0x00009400);
	regmap_write(bach->bach, 0x44, 0x00009400);
	regmap_write(bach->bach, 0x48, 0x00009400);
	regmap_write(bach->bach, 0x4C, 0x0000D400);
	regmap_write(bach->bach, 0x50, 0x00008400);
	regmap_write(bach->bach, 0x54, 0x0000D000);
	regmap_write(bach->bach, 0x58, 0x00009400);
	regmap_write(bach->bach, 0x5C, 0x00009400);
	regmap_write(bach->bach, 0x60, 0x00008400);
	regmap_write(bach->bach, 0x64, 0x00000000);
	regmap_write(bach->bach, 0x68, 0x00000000);
	regmap_write(bach->bach, 0x6C, 0x00000000);
	regmap_write(bach->bach, 0x70, 0x00000000);
	regmap_write(bach->bach, 0x74, 0x00000000);
	regmap_write(bach->bach, 0x78, 0x00000000);
	regmap_write(bach->bach, 0x7C, 0x00000000);
	regmap_write(bach->bach, 0x80, 0x00000005);
	//regmap_write(bach->bach, 0x84, 0x0000ECEC;
	regmap_write(bach->bach, 0x88, 0x00000007);
	regmap_write(bach->bach, 0x8C, 0x00000000);
	regmap_write(bach->bach, 0x90, 0x00000037);
	regmap_write(bach->bach, 0x94, 0x00000000);
	regmap_write(bach->bach, 0x98, 0x00000007);
	regmap_write(bach->bach, 0x9C, 0x00000000);
	regmap_write(bach->bach, 0xA0, 0x00000037);
	regmap_write(bach->bach, 0xA4, 0x00000000);
	regmap_write(bach->bach, 0xA8, 0x00000007);
	regmap_write(bach->bach, 0xAC, 0x00000000);
	regmap_write(bach->bach, 0xB0, 0x00000007);
	regmap_write(bach->bach, 0xB4, 0x00000000);
	regmap_write(bach->bach, 0xB8, 0x00000007);
	regmap_write(bach->bach, 0xBC, 0x00000000);
	regmap_write(bach->bach, 0xC0, 0x00000037);
	regmap_write(bach->bach, 0xC4, 0x00000000);
	regmap_write(bach->bach, 0xC8, 0x00000007);
	regmap_write(bach->bach, 0xCC, 0x00000000);
	regmap_write(bach->bach, 0xD0, 0x00000000);
	regmap_write(bach->bach, 0xD4, 0x00000000);
	regmap_write(bach->bach, 0xD8, 0x00000000);
	regmap_write(bach->bach, 0xDC, 0x00000000);
	regmap_write(bach->bach, 0xE0, 0x00000000);
	regmap_write(bach->bach, 0xE4, 0x00000000);
	regmap_write(bach->bach, 0xE8, 0x00000000);
	regmap_write(bach->bach, 0xEC, 0x00000000);
	regmap_write(bach->bach, 0xF0, 0x00000000);
	regmap_write(bach->bach, 0xF4, 0x00000000);
	regmap_write(bach->bach, 0xF8, 0x00000000);
	regmap_write(bach->bach, 0xFC, 0x00000000);
	//#regmap_write(bach->bach, 0x100, 0x00000496;
	//#regmap_write(bach->bach, 0x104, 0x00008000);
	regmap_write(bach->bach, 0x108, 0x00000FE8);
	regmap_write(bach->bach, 0x10C, 0x00002000);
	regmap_write(bach->bach, 0x110, 0x00000800);
	regmap_write(bach->bach, 0x114, 0x00000000);
	regmap_write(bach->bach, 0x118, 0x00001FE0);
	regmap_write(bach->bach, 0x11C, 0x00000F88);
	regmap_write(bach->bach, 0x120, 0x0000000F);
	regmap_write(bach->bach, 0x124, 0x00000000);
	regmap_write(bach->bach, 0x128, 0x00000000);
	regmap_write(bach->bach, 0x12C, 0x00000000);
	regmap_write(bach->bach, 0x130, 0x00000000);
	regmap_write(bach->bach, 0x134, 0x00000000);
	regmap_write(bach->bach, 0x138, 0x00000000);
	regmap_write(bach->bach, 0x13C, 0x00000000);
	regmap_write(bach->bach, 0x140, 0x00000000);
	regmap_write(bach->bach, 0x144, 0x00000000);
	regmap_write(bach->bach, 0x148, 0x00000000);
	regmap_write(bach->bach, 0x14C, 0x00000000);
	regmap_write(bach->bach, 0x150, 0x00000000);
	regmap_write(bach->bach, 0x154, 0x00000000);
	regmap_write(bach->bach, 0x158, 0x00000000);
	regmap_write(bach->bach, 0x15C, 0x00000000);
	regmap_write(bach->bach, 0x160, 0x00000000);
	regmap_write(bach->bach, 0x164, 0x00000000);
	regmap_write(bach->bach, 0x168, 0x00000000);
	regmap_write(bach->bach, 0x16C, 0x00000000);
	regmap_write(bach->bach, 0x170, 0x00000000);
	regmap_write(bach->bach, 0x174, 0x00000000);
	regmap_write(bach->bach, 0x178, 0x00000000);
	regmap_write(bach->bach, 0x17C, 0x00000000);
	regmap_write(bach->bach, 0x180, 0x00000000);
	regmap_write(bach->bach, 0x184, 0x00000000);
	regmap_write(bach->bach, 0x188, 0x00000000);
	regmap_write(bach->bach, 0x18C, 0x00000000);
	regmap_write(bach->bach, 0x190, 0x00000000);
	regmap_write(bach->bach, 0x194, 0x00000000);
	regmap_write(bach->bach, 0x198, 0x00000000);
	regmap_write(bach->bach, 0x19C, 0x00000000);
	regmap_write(bach->bach, 0x1A0, 0x00000000);
	regmap_write(bach->bach, 0x1A4, 0x00000000);
	regmap_write(bach->bach, 0x1A8, 0x00000000);
	regmap_write(bach->bach, 0x1AC, 0x00000000);
	regmap_write(bach->bach, 0x1B0, 0x00000000);
	regmap_write(bach->bach, 0x1B4, 0x00000000);
	regmap_write(bach->bach, 0x1B8, 0x00000000);
	regmap_write(bach->bach, 0x1BC, 0x00000000);
	regmap_write(bach->bach, 0x1C0, 0x00000000);
	regmap_write(bach->bach, 0x1C4, 0x00000000);
	regmap_write(bach->bach, 0x1C8, 0x00000000);
	regmap_write(bach->bach, 0x1CC, 0x000000E3);
	regmap_write(bach->bach, 0x1D0, 0x00000097);
	regmap_write(bach->bach, 0x1D4, 0x00000000);
	regmap_write(bach->bach, 0x1D8, 0x00000000);
	regmap_write(bach->bach, 0x1DC, 0x00000400);
	regmap_write(bach->bach, 0x1E0, 0x00000000);
	regmap_write(bach->bach, 0x1E4, 0x00000000);
	regmap_write(bach->bach, 0x1E8, 0x00000000);
	regmap_write(bach->bach, 0x1EC, 0x00000000);
	regmap_write(bach->bach, 0x1F0, 0x00000000);
	regmap_write(bach->bach, 0x1F4, 0x00000000);
	regmap_write(bach->bach, 0x1F8, 0x00000000);
	regmap_write(bach->bach, 0x1FC, 0x00000000);
	regmap_write(bach->bach, 0x200, 0x00000000);
	regmap_write(bach->bach, 0x204, 0x00000000);
	regmap_write(bach->bach, 0x208, 0x00000000);
	regmap_write(bach->bach, 0x20C, 0x00000000);
	regmap_write(bach->bach, 0x210, 0x00004000);
	regmap_write(bach->bach, 0x214, 0x00000100);
	regmap_write(bach->bach, 0x218, 0x000003E8);
	//#regmap_write(bach->bach, 0x21C, 0x00000002;
	regmap_write(bach->bach, 0x220, 0x00000000);
	regmap_write(bach->bach, 0x224, 0x00000000);
	regmap_write(bach->bach, 0x228, 0x00000000);
	regmap_write(bach->bach, 0x22C, 0x00000000);
	regmap_write(bach->bach, 0x230, 0x00000000);
	regmap_write(bach->bach, 0x234, 0x00000000);
	regmap_write(bach->bach, 0x238, 0x00000003);
	regmap_write(bach->bach, 0x23C, 0x00000000);
	regmap_write(bach->bach, 0x240, 0x000038C0);
	regmap_write(bach->bach, 0x244, 0x00003838);
	regmap_write(bach->bach, 0x248, 0x00000C04);
	regmap_write(bach->bach, 0x24C, 0x00001C14);
	regmap_write(bach->bach, 0x250, 0x00000001);
	regmap_write(bach->bach, 0x254, 0x00000000);
	regmap_write(bach->bach, 0x258, 0x00000003);
	regmap_write(bach->bach, 0x25C, 0x00000000);
	regmap_write(bach->bach, 0x260, 0x00000000);
	regmap_write(bach->bach, 0x264, 0x00000000);
	regmap_write(bach->bach, 0x268, 0x00000000);
	regmap_write(bach->bach, 0x26C, 0x00000202);
	regmap_write(bach->bach, 0x270, 0x00000000);
	regmap_write(bach->bach, 0x274, 0x00000000);
	regmap_write(bach->bach, 0x278, 0x00000000);
	regmap_write(bach->bach, 0x27C, 0x00000000);
	regmap_write(bach->bach, 0x280, 0x00000000);
	regmap_write(bach->bach, 0x284, 0x00000000);
	regmap_write(bach->bach, 0x288, 0x00000000);
	regmap_write(bach->bach, 0x28C, 0x00000000);
	regmap_write(bach->bach, 0x290, 0x00000000);
	regmap_write(bach->bach, 0x294, 0x00001234);
	regmap_write(bach->bach, 0x298, 0x00005678);
	regmap_write(bach->bach, 0x29C, 0x00000000);
	regmap_write(bach->bach, 0x2A0, 0x00000000);
	regmap_write(bach->bach, 0x2A4, 0x00000000);
	regmap_write(bach->bach, 0x2A8, 0x00000000);
	regmap_write(bach->bach, 0x2AC, 0x00000000);
	regmap_write(bach->bach, 0x2B0, 0x00000000);
	regmap_write(bach->bach, 0x2B4, 0x00000000);
	regmap_write(bach->bach, 0x2B8, 0x00000000);
	regmap_write(bach->bach, 0x2BC, 0x00000000);
	regmap_write(bach->bach, 0x2C0, 0x00000000);
	regmap_write(bach->bach, 0x2C4, 0x00000000);
	regmap_write(bach->bach, 0x2C8, 0x00000000);
	regmap_write(bach->bach, 0x2CC, 0x00000000);
	regmap_write(bach->bach, 0x2D0, 0x00000000);
	regmap_write(bach->bach, 0x2D4, 0x00000000);
	regmap_write(bach->bach, 0x2D8, 0x00000000);
	regmap_write(bach->bach, 0x2DC, 0x00000000);
	regmap_write(bach->bach, 0x2E0, 0x00000000);
	regmap_write(bach->bach, 0x2E4, 0x00000000);
	regmap_write(bach->bach, 0x2E8, 0x00000000);
	regmap_write(bach->bach, 0x2EC, 0x00000000);
	regmap_write(bach->bach, 0x2F0, 0x00000000);
	regmap_write(bach->bach, 0x2F4, 0x00000000);
	regmap_write(bach->bach, 0x2F8, 0x00000000);
	regmap_write(bach->bach, 0x2FC, 0x00000000);
	regmap_write(bach->bach, 0x300, 0x00000000);
	regmap_write(bach->bach, 0x304, 0x00000000);
	regmap_write(bach->bach, 0x308, 0x00000000);
	regmap_write(bach->bach, 0x30C, 0x00000000);
	regmap_write(bach->bach, 0x310, 0x00000000);
	regmap_write(bach->bach, 0x314, 0x00000000);
	regmap_write(bach->bach, 0x318, 0x00000000);
	regmap_write(bach->bach, 0x31C, 0x00000000);
	regmap_write(bach->bach, 0x320, 0x00000000);
	regmap_write(bach->bach, 0x324, 0x00000000);
	regmap_write(bach->bach, 0x328, 0x00000000);
	regmap_write(bach->bach, 0x32C, 0x00000001);
	regmap_write(bach->bach, 0x330, 0x00000000);
	regmap_write(bach->bach, 0x334, 0x00000000);
	regmap_write(bach->bach, 0x338, 0x00000000);
	regmap_write(bach->bach, 0x33C, 0x00000000);
	regmap_write(bach->bach, 0x340, 0x00000000);
	regmap_write(bach->bach, 0x344, 0x00000000);
	regmap_write(bach->bach, 0x348, 0x00000000);
	regmap_write(bach->bach, 0x34C, 0x00000000);
	regmap_write(bach->bach, 0x350, 0x00000000);
	regmap_write(bach->bach, 0x354, 0x00000000);
	regmap_write(bach->bach, 0x358, 0x00000000);
	regmap_write(bach->bach, 0x35C, 0x00000000);
	regmap_write(bach->bach, 0x360, 0x00000000);
	regmap_write(bach->bach, 0x364, 0x00000000);
	regmap_write(bach->bach, 0x368, 0x00000000);
	regmap_write(bach->bach, 0x36C, 0x00000000);
	regmap_write(bach->bach, 0x370, 0x00000000);
	regmap_write(bach->bach, 0x374, 0x00000000);
	regmap_write(bach->bach, 0x378, 0x00000000);
	regmap_write(bach->bach, 0x37C, 0x00000080);
	regmap_write(bach->bach, 0x380, 0x00000000);
	regmap_write(bach->bach, 0x384, 0x00000000);
	regmap_write(bach->bach, 0x388, 0x0000FF34);
	regmap_write(bach->bach, 0x38C, 0x00000000);
	regmap_write(bach->bach, 0x390, 0x00007FFF);
	regmap_write(bach->bach, 0x394, 0x00007FE9);
	regmap_write(bach->bach, 0x398, 0x00000000);
	regmap_write(bach->bach, 0x39C, 0x00000000);
	regmap_write(bach->bach, 0x3A0, 0x00000000);
	regmap_write(bach->bach, 0x3A4, 0x00000000);
	regmap_write(bach->bach, 0x3A8, 0x00000000);
	regmap_write(bach->bach, 0x3AC, 0x0000FEA6);
	regmap_write(bach->bach, 0x3B0, 0x0000019D);
	regmap_write(bach->bach, 0x3B4, 0x00000000);
	regmap_write(bach->bach, 0x3B8, 0x00000000);
	regmap_write(bach->bach, 0x3BC, 0x000078F4);
	regmap_write(bach->bach, 0x3C0, 0x00000000);
	regmap_write(bach->bach, 0x3C4, 0x00000000);
	regmap_write(bach->bach, 0x3C8, 0x000010D3);
	regmap_write(bach->bach, 0x3CC, 0x00000942);
	regmap_write(bach->bach, 0x3D0, 0x00000000);
	regmap_write(bach->bach, 0x3D4, 0x00000000);
	regmap_write(bach->bach, 0x3D8, 0x0000FDB6);
	regmap_write(bach->bach, 0x3DC, 0x0000F291);
	regmap_write(bach->bach, 0x3E0, 0x000078F4);
	regmap_write(bach->bach, 0x3E4, 0x00000000);
	regmap_write(bach->bach, 0x3E8, 0x00000000);
	regmap_write(bach->bach, 0x3EC, 0x00000000);
	regmap_write(bach->bach, 0x3F0, 0x00007FFF);
	regmap_write(bach->bach, 0x3F4, 0x00000000);
	regmap_write(bach->bach, 0x3F8, 0x00000001);
	regmap_write(bach->bach, 0x3FC, 0x00000000);
	regmap_write(bach->bach, 0x400, 0x00000000);
	regmap_write(bach->bach, 0x404, 0x00000021);
	regmap_write(bach->bach, 0x408, 0x00000000);
	regmap_write(bach->bach, 0x40C, 0x00000000);
	regmap_write(bach->bach, 0x410, 0x0000000A);
	regmap_write(bach->bach, 0x414, 0x00008000);
	regmap_write(bach->bach, 0x418, 0x0000011F);
	regmap_write(bach->bach, 0x41C, 0x00000000);
	regmap_write(bach->bach, 0x420, 0x00000000);
	regmap_write(bach->bach, 0x424, 0x00000000);
	regmap_write(bach->bach, 0x428, 0x00000000);
	regmap_write(bach->bach, 0x42C, 0x00000000);
	regmap_write(bach->bach, 0x430, 0x00000000);
	regmap_write(bach->bach, 0x434, 0x00000000);
	regmap_write(bach->bach, 0x438, 0x00000000);
	regmap_write(bach->bach, 0x43C, 0x0000FFFF);
	regmap_write(bach->bach, 0x440, 0x00000000);
	regmap_write(bach->bach, 0x444, 0x00000001);
	regmap_write(bach->bach, 0x448, 0x00008000);
	regmap_write(bach->bach, 0x44C, 0x00000001);
	regmap_write(bach->bach, 0x450, 0x00008000);
	regmap_write(bach->bach, 0x454, 0x00000000);
	regmap_write(bach->bach, 0x458, 0x00000000);
	regmap_write(bach->bach, 0x45C, 0x00000000);
	regmap_write(bach->bach, 0x460, 0x00000000);
	regmap_write(bach->bach, 0x464, 0x00000000);
	regmap_write(bach->bach, 0x468, 0x00000000);
	regmap_write(bach->bach, 0x46C, 0x00000000);
	regmap_write(bach->bach, 0x470, 0x00000000);
	regmap_write(bach->bach, 0x474, 0x00000000);
	regmap_write(bach->bach, 0x478, 0x00000000);
	regmap_write(bach->bach, 0x47C, 0x00000000);
	regmap_write(bach->bach, 0x480, 0x00000001);
	regmap_write(bach->bach, 0x484, 0x00000000);
	regmap_write(bach->bach, 0x488, 0x00000000);
	regmap_write(bach->bach, 0x48C, 0x00000000);
	regmap_write(bach->bach, 0x490, 0x00000000);
	regmap_write(bach->bach, 0x494, 0x00000000);
	regmap_write(bach->bach, 0x498, 0x00000000);
	regmap_write(bach->bach, 0x49C, 0x00000000);
	regmap_write(bach->bach, 0x4A0, 0x00000000);
	regmap_write(bach->bach, 0x4A4, 0x00000000);
	regmap_write(bach->bach, 0x4A8, 0x00000000);
	regmap_write(bach->bach, 0x4AC, 0x00000000);
	regmap_write(bach->bach, 0x4B0, 0x00000000);
	regmap_write(bach->bach, 0x4B4, 0x00000000);
	regmap_write(bach->bach, 0x4B8, 0x00000000);
	regmap_write(bach->bach, 0x4BC, 0x00000000);
	regmap_write(bach->bach, 0x4C0, 0x00000000);
	regmap_write(bach->bach, 0x4C4, 0x00000000);
	regmap_write(bach->bach, 0x4C8, 0x00000000);
	regmap_write(bach->bach, 0x4CC, 0x00000000);
	regmap_write(bach->bach, 0x4D0, 0x00000000);
	regmap_write(bach->bach, 0x4D4, 0x00000000);
	regmap_write(bach->bach, 0x4D8, 0x00000000);
	regmap_write(bach->bach, 0x4DC, 0x00000000);
	regmap_write(bach->bach, 0x4E0, 0x00000000);
	regmap_write(bach->bach, 0x4E4, 0x00000000);
	regmap_write(bach->bach, 0x4E8, 0x00000000);
	regmap_write(bach->bach, 0x4EC, 0x00000000);
	regmap_write(bach->bach, 0x4F0, 0x00000000);
	regmap_write(bach->bach, 0x4F4, 0x00000000);
	regmap_write(bach->bach, 0x4F8, 0x00000000);
	regmap_write(bach->bach, 0x4FC, 0x00000000);
	regmap_write(bach->bach, 0x500, 0x00000080);
	regmap_write(bach->bach, 0x504, 0x00000078);
	regmap_write(bach->bach, 0x508, 0x00000000);
	regmap_write(bach->bach, 0x50C, 0x00000000);
	regmap_write(bach->bach, 0x510, 0x00000000);
	regmap_write(bach->bach, 0x514, 0x00000000);
	regmap_write(bach->bach, 0x518, 0x00000000);
	regmap_write(bach->bach, 0x51C, 0x00000000);
	regmap_write(bach->bach, 0x520, 0x00000000);
	regmap_write(bach->bach, 0x524, 0x00000000);
	regmap_write(bach->bach, 0x528, 0x00000000);
	regmap_write(bach->bach, 0x52C, 0x00000000);
	regmap_write(bach->bach, 0x530, 0x00000000);
	regmap_write(bach->bach, 0x534, 0x00000000);
	regmap_write(bach->bach, 0x538, 0x00000000);
	regmap_write(bach->bach, 0x53C, 0x00000000);
	regmap_write(bach->bach, 0x540, 0x00000000);
	regmap_write(bach->bach, 0x544, 0x00000000);
	regmap_write(bach->bach, 0x548, 0x00000000);
	regmap_write(bach->bach, 0x54C, 0x00000000);
	regmap_write(bach->bach, 0x550, 0x00000000);
	regmap_write(bach->bach, 0x554, 0x00000000);
	regmap_write(bach->bach, 0x558, 0x00000000);
	regmap_write(bach->bach, 0x55C, 0x00000000);
	regmap_write(bach->bach, 0x560, 0x00000000);
	regmap_write(bach->bach, 0x564, 0x00000000);
	regmap_write(bach->bach, 0x568, 0x00000000);
	regmap_write(bach->bach, 0x56C, 0x00000000);
	regmap_write(bach->bach, 0x570, 0x00000000);
	regmap_write(bach->bach, 0x574, 0x00000000);
	regmap_write(bach->bach, 0x578, 0x00000000);
	regmap_write(bach->bach, 0x57C, 0x00000000);
	regmap_write(bach->bach, 0x580, 0x00000000);
	regmap_write(bach->bach, 0x584, 0x00000000);
	regmap_write(bach->bach, 0x588, 0x00000000);
	regmap_write(bach->bach, 0x58C, 0x00000000);
	regmap_write(bach->bach, 0x590, 0x00000000);
	regmap_write(bach->bach, 0x594, 0x00000000);
	regmap_write(bach->bach, 0x598, 0x00000000);
	regmap_write(bach->bach, 0x59C, 0x00000000);
	regmap_write(bach->bach, 0x5A0, 0x00000000);
	regmap_write(bach->bach, 0x5A4, 0x00000000);
	regmap_write(bach->bach, 0x5A8, 0x00000000);
	regmap_write(bach->bach, 0x5AC, 0x00000000);
	regmap_write(bach->bach, 0x5B0, 0x00000000);
	regmap_write(bach->bach, 0x5B4, 0x00000000);
	regmap_write(bach->bach, 0x5B8, 0x00000000);
	regmap_write(bach->bach, 0x5BC, 0x00000000);
	regmap_write(bach->bach, 0x5C0, 0x00000000);
	regmap_write(bach->bach, 0x5C4, 0x00000B0B);
	regmap_write(bach->bach, 0x5C8, 0x00000000);
	regmap_write(bach->bach, 0x5CC, 0x00004A4A);
	regmap_read(bach->bach, 0x5CC, &ret_val);
	
	regmap_read(bach->bach, 0x5CC, &ret_val);
	
	regmap_write(bach->bach, 0x5D0, 0x00004A4A);
	regmap_write(bach->bach, 0x5D4, 0x00000000);
	regmap_write(bach->bach, 0x5D8, 0x00000000);
	regmap_write(bach->bach, 0x5DC, 0x00004949);
	regmap_read(bach->bach, 0x5DC, &ret_val);
	
	regmap_write(bach->bach, 0x5E0, 0x00004949);
	regmap_read(bach->bach, 0x5E0, &ret_val);
	
	regmap_write(bach->bach, 0x5E4, 0x00000000);
	regmap_write(bach->bach, 0x5E8, 0x00000000);
	regmap_write(bach->bach, 0x5EC, 0x00000000);
	regmap_write(bach->bach, 0x5F0, 0x00000000);
	regmap_write(bach->bach, 0x5F4, 0x00000000);
	regmap_write(bach->bach, 0x5F8, 0x00000000);
	regmap_write(bach->bach, 0x5FC, 0x00000000);
}


static int io_mmap(struct msc313_bach *bach)
{
	_mmio_base = mmio_map();
	bach->clk = _mmio_base + MSC313_BACH_CLK;
	bach->bach = _mmio_base + MSC313_BACH;
	bach->audiotop = _mmio_base + MSC313_BACH_TOP;
	
	return 0;
}

static int enable_clk(unsigned int clk_base)
{
	WRITE_WORD(clk_base + 0x0, 0x00c0);
	WRITE_BYTE(clk_base + 0x1c, 0x01);
	return 0;
}

static int pre_allocate_dma_buffer(struct msc313_bach *bach)
{
	unsigned int vaddr = dma_alloc(0, PRE_ALLOCATED_PCM_BUF_MAX_SIZE);
	if (vaddr == 0) {
		
		bach->dma_areas = 0;
		return -ENOMEM;
	}
	bach->dma_areas = vaddr;
	return 0;
}

static int msc313_mem_dai_init(struct snd_soc_dai *dai)
{
	
	struct msc313_bach *bach;
	unsigned int i, j;
	struct reg_field src2_sel_field = REG_FIELD(MSC313_BACH_SR0_SEL, 0, 3);
	struct reg_field src1_sel_field = REG_FIELD(MSC313_BACH_SR0_SEL, 4, 7);
	struct reg_field dma1_rd_mono_field = REG_FIELD(MSC313_BACH_DMA_TEST_CTRL7, 15, 15);
	struct reg_field dma1_wr_mono_field = REG_FIELD(MSC313_BACH_DMA_TEST_CTRL7, 14, 14);
	struct reg_field dma1_rd_mono_copy_field = REG_FIELD(MSC313_BACH_DMA_TEST_CTRL7, 13, 13);
	struct reg_field dma_int_en_field = REG_FIELD(REG_DMA_INT, 1, 1);

	bach = calloc(1, sizeof(*bach));
	if(bach == NULL) {
		
		return -ENOMEM;
	}

	io_mmap(bach);
	enable_clk(bach->clk);
	/* pre allocated dma memory */
	if (pre_allocate_dma_buffer(bach) != 0) {
		/*
		 * Without a usable DMA area, allocating regmap_fields is
		 * pointless: every later hw_params would feed an address
		 * of 0 into the DMA engine. Bail out so audctrl can fail
		 * fast instead of going through a half-initialised bach.
		 */
		free(bach);
		return -ENOMEM;
	}

	bach->src1_sel = regmap_field_alloc(bach->bach, src1_sel_field);
	bach->src2_sel = regmap_field_alloc(bach->bach, src2_sel_field);
	bach->dma_int_en = regmap_field_alloc(bach->bach, dma_int_en_field);

	for (i = 0; i < ARRAY_SIZE(bach->dma_channels); i++) {
		struct msc313_bach_dma_channel *chan = &bach->dma_channels[i];
		unsigned int chan_offset = 0x100 + (0x40 * i);
		struct reg_field chan_rst_field = REG_FIELD(chan_offset + MSC313_BACH_DMA_CHANNEL_CTRL0, 0, 0);
		struct reg_field chan_en_field = REG_FIELD(chan_offset + MSC313_BACH_DMA_CHANNEL_CTRL0, 1, 1);
		struct reg_field live_count_en_field = REG_FIELD(chan_offset + MSC313_BACH_DMA_CHANNEL_CTRL0, 2, 2);

		/* interrupt controls */
		struct reg_field chan_rd_int_clear_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL0, 8, 8);
		struct reg_field chan_rd_empty_int_en_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL0, 10, 10);
		struct reg_field chan_rd_overrun_int_en_field = REG_FIELD(chan_offset +
						MSC313_BACH_DMA_CHANNEL_CTRL0, 12, 12);
		struct reg_field chan_rd_underrun_int_en_field = REG_FIELD(chan_offset +
						MSC313_BACH_DMA_CHANNEL_CTRL0, 13, 13);

		/* flags */
		struct reg_field chan_wd_underrun_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 0, 0);
		struct reg_field chan_wd_overrun_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 1, 1);
		struct reg_field chan_rd_underrun_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 2, 2);
		struct reg_field chan_rd_overrun_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 3, 3);
		struct reg_field chan_rd_empty_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 4, 4);
		struct reg_field chan_wr_full_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 5, 5);
		struct reg_field chan_wr_localbuf_full_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 6, 6);
		struct reg_field chan_rd_localbuf_empty_flag_field = REG_FIELD(chan_offset +
				MSC313_BACH_DMA_CHANNEL_CTRL8, 7, 7);

		//spin_lock_init(&chan->lock);

		chan->rst = regmap_field_alloc(bach->bach, chan_rst_field);
		chan->en = regmap_field_alloc(bach->bach, chan_en_field);
		chan->live_count_en = regmap_field_alloc(bach->bach, live_count_en_field);
		chan->rd_int_clear = regmap_field_alloc(bach->bach, chan_rd_int_clear_field);
		chan->rd_empty_int_en = regmap_field_alloc(bach->bach, chan_rd_empty_int_en_field);
		chan->rd_overrun_int_en = regmap_field_alloc(bach->bach, chan_rd_overrun_int_en_field);
		chan->rd_underrun_int_en = regmap_field_alloc(bach->bach, chan_rd_underrun_int_en_field);

		chan->wr_underrun_flag = regmap_field_alloc(bach->bach, chan_wd_underrun_flag_field);
		chan->wr_overrun_flag = regmap_field_alloc(bach->bach, chan_wd_overrun_flag_field);
		chan->rd_underrun_flag = regmap_field_alloc(bach->bach, chan_rd_underrun_flag_field);
		chan->rd_overrun_flag = regmap_field_alloc(bach->bach, chan_rd_overrun_flag_field);
		chan->rd_empty_flag = regmap_field_alloc(bach->bach, chan_rd_empty_flag_field);
		chan->wr_full_flag = regmap_field_alloc(bach->bach, chan_wr_full_flag_field);
		chan->wr_localbuf_full_flag = regmap_field_alloc(bach->bach, chan_wr_localbuf_full_flag_field);
		chan->rd_localbuf_empty_flag = regmap_field_alloc(bach->bach, chan_rd_localbuf_empty_flag_field);

		if (i == 0) {
			chan->dma_rd_mono = regmap_field_alloc(bach->bach, dma1_rd_mono_field);
			chan->dma_wr_mono = regmap_field_alloc(bach->bach, dma1_wr_mono_field);
			chan->dma_rd_mono_copy = regmap_field_alloc(bach->bach, dma1_rd_mono_copy_field);
		}

		for (j = 0; j < ARRAY_SIZE(chan->reader_writer); j++){
			struct msc313_bach_dma_sub_channel *sub = &chan->reader_writer[j];
			unsigned int sub_chan_offset = chan_offset + 4 + (0x20 * j);

			sub->dma_channel = chan;

			/* Sub channel ctrl  fields */
			struct reg_field sub_chan_count_field = REG_FIELD(sub_chan_offset +
								MSC313_BACH_DMA_SUB_CHANNEL_EN, 12, 12);
			struct reg_field sub_chan_trigger_field = REG_FIELD(sub_chan_offset +
					MSC313_BACH_DMA_SUB_CHANNEL_EN, 13, 13);
			struct reg_field sub_chan_init_field = REG_FIELD(sub_chan_offset +
								MSC313_BACH_DMA_SUB_CHANNEL_EN, 14, 14);
			struct reg_field sub_chan_en_field = REG_FIELD(sub_chan_offset +
					MSC313_BACH_DMA_SUB_CHANNEL_EN, 15, 15);
			/* Buffer address */
			struct reg_field sub_chan_addr_lo_field = REG_FIELD(sub_chan_offset +
								MSC313_BACH_DMA_SUB_CHANNEL_EN, 0, 11);
			struct reg_field sub_chan_addr_hi_field = REG_FIELD(sub_chan_offset +
								MSC313_BACH_DMA_SUB_CHANNEL_ADDR, 0, 14);
			/* The rest .. */
			struct reg_field sub_chan_size_field = REG_FIELD(sub_chan_offset +
					MSC313_BACH_DMA_SUB_CHANNEL_SIZE, 0, 15);
			struct reg_field sub_chan_trigger_level_field = REG_FIELD(sub_chan_offset +
					MSC313_BACH_DMA_SUB_CHANNEL_TRIGGER, 0, 15);
			struct reg_field sub_chan_overrunthreshold_field = REG_FIELD(sub_chan_offset + 0x10, 0, 15);
			struct reg_field sub_chan_underrunthreshold_field = REG_FIELD(sub_chan_offset + 0x14, 0, 15);
			struct reg_field sub_chan_level_field = REG_FIELD(sub_chan_offset +
					MSC313_BACK_DMA_SUB_CHANNEL_LEVEL, 0, 15);

			sub->count = regmap_field_alloc(bach->bach, sub_chan_count_field);
			sub->trigger = regmap_field_alloc(bach->bach, sub_chan_trigger_field);
			sub->init = regmap_field_alloc(bach->bach, sub_chan_init_field);
			sub->en = regmap_field_alloc(bach->bach, sub_chan_en_field);
			sub->addr_hi = regmap_field_alloc(bach->bach, sub_chan_addr_hi_field);
			sub->addr_lo = regmap_field_alloc(bach->bach, sub_chan_addr_lo_field);
			sub->size = regmap_field_alloc(bach->bach, sub_chan_size_field);
			sub->trigger_level = regmap_field_alloc(bach->bach, sub_chan_trigger_level_field);
			sub->overrunthreshold = regmap_field_alloc(bach->bach, sub_chan_overrunthreshold_field);
			sub->underrunthreshold = regmap_field_alloc(bach->bach, sub_chan_underrunthreshold_field);
			sub->level = regmap_field_alloc(bach->bach, sub_chan_level_field);

			regmap_field_write(sub->en, 0);
		}

		regmap_field_write(chan->en, 0);
		regmap_field_write(chan->rst, 1);
	}

	dai->private_data = bach;

	regmap_field_write(bach->dma_int_en, 1);

	msc313_bach_the_horror(bach);

	
	return 0;
}


struct snd_soc_dai msc_dais[] = {
	[0] = {
		.name = "msc313-bach-cpu-dai",
		.type = DAI_TYPE_MEMIF,
		.ops = &msc313_cpu_dai_ops,
	}
};

static int msc313_add_dais_internal(struct snd_pcm *pcm, struct snd_soc_dai *dais, int dai_num)
{
	int index = 0;
	struct snd_soc_dai *mem_dai = NULL;

	if (dai_num == 0) {
		return 0;
	}

	for (index = 0; index < dai_num; index++) {
		struct snd_soc_dai *dai = &dais[index];
		list_init(&dai->list);
		snd_add_dai(pcm, dai, dai->type);

		if (dai->type == DAI_TYPE_MEMIF) {
			mem_dai = dai;
		}
	}

	if (mem_dai != NULL) {
		msc313_mem_dai_init(mem_dai);
	}
	return 0;
}

int msc313_add_dais(struct snd_pcm *pcm)
{
	return msc313_add_dais_internal(pcm, msc_dais, ARRAY_SIZE(msc_dais));
}
