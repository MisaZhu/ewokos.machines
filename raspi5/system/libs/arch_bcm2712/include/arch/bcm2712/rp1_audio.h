#ifndef BCM2712_RP1_AUDIO_H
#define BCM2712_RP1_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * RP1 audio_out playback backend for BCM2712 (Raspberry Pi 5).
 *
 * The Pi 5 has no BCM283x-style PWM, clock manager or DMA controller any
 * more: everything the old soundpwmd driver talked to is gone. Stereo audio
 * out on the Pi 5 lives in the RP1 southbridge as the "audio_out" block
 * (rp1.dtsi audio_out@94000, "raspberrypi,rp1-audio-out"), which is not a
 * plain PWM at all but a 2-channel sigma-delta modulator feeding a
 * 40-times-oversampled, 40-level two-sided PWM stage. Samples reach it over
 * the RP1 DW AXI DMAC (dma@188000, "snps,axi-dma-1.01a").
 *
 * Consequences for callers:
 *
 *  - The format is fixed by the hardware tuning: 48 kHz, 2 channels,
 *    S16_LE. rp1_aout.c rejects every other hw_params combination, and the
 *    clock tree (153.6 MHz = 48000 * 40 * 80) only works out for 48 kHz.
 *
 *  - Exactly ONE 32-bit FIFO word per stereo frame. The FIFO word is the
 *    little-endian load of the interleaved S16_LE pair, so bits [15:0] are
 *    LEFT and bits [31:16] are RIGHT. rp1_audio_pack_frame() builds it.
 *
 *  - Playback is a free-running circular DMA ring. Once started, the DMAC
 *    walks the ring forever and never stops on its own; there is no
 *    "transfer complete". Callers must keep every slot filled (silence on
 *    underrun) and must keep the ring position polled, see below.
 *
 * Ring model, and why it differs from Linux/Circle:
 *
 *   Both the linux driver and Circle set CTL_H.LLI_LAST on *every* linked
 *   list item, so the controller disables the channel at each block
 *   boundary and their interrupt handlers re-assert DMAC_CHEN and re-set
 *   CTL_H.LLI_VALID (the controller clears LLI_VALID when it writes the
 *   block status back into the LLI). EwokOS user space has no interrupt
 *   handlers, and the audio_out FIFO only covers ~666us of audio against a
 *   500us..1ms feeder loop, so that model would glitch constantly.
 *
 *   Here LLI_LAST stays clear and the ring is closed (the last LLI links
 *   back to the first), so the controller keeps fetching forever. LLI_VALID
 *   still has to be re-armed after each lap: rp1_audio_slot_commit() does
 *   it, and it is always safe because a slot is only committed while the
 *   controller is at least RP1_AUDIO_GUARD_SLOTS away from it.
 *
 *   Position comes from CH_LLP, which the controller updates as it walks
 *   the ring; linux reads the same register back in
 *   axi_chan_block_xfer_complete() to find out which block finished.
 */

#define RP1_AUDIO_RATE        48000u
#define RP1_AUDIO_CHANNELS    2u
#define RP1_AUDIO_BITS        16u
/* one 32-bit FIFO word per stereo frame */
#define RP1_AUDIO_FRAME_WORDS 1u

/*
 * Default ring geometry: 16 slots of 512 frames = 2 KB per slot, i.e.
 * 10.67 ms per slot and ~170 ms of buffered audio. That is the same order
 * as the start/rebuffer targets the old bcm283x engine used (160 ms /
 * 224 ms), which keeps the feeder's sleep cadence and the IPC ring sizing
 * unchanged.
 */
#define RP1_AUDIO_DEF_SLOTS       16u
#define RP1_AUDIO_DEF_SLOT_FRAMES 512u

/*
 * Guard band around the slot the controller is working on. CH_LLP is read
 * without being able to tell whether the controller has already fetched the
 * following LLI, so the slots on both sides of the reported one are treated
 * as in flight as well. With 16 slots that still leaves 11 writable, i.e.
 * ~117 ms of headroom against a 500 us..1 ms feeder loop.
 */
#define RP1_AUDIO_GUARD_SLOTS 2u

/* init flags */
#define RP1_AUDIO_F_CHANNEL_SWAP (1u << 0)

/* error codes returned by rp1_audio_init()/rp1_audio_start() */
#define RP1_AUDIO_ERR_NONE      0
#define RP1_AUDIO_ERR_MAP      -1   /* SYS_MEM_MAP refused a window */
#define RP1_AUDIO_ERR_RP1      -2   /* bcm2712_rp1_init() failed */
#define RP1_AUDIO_ERR_CLOCK    -3   /* PLL_AUDIO_CORE never locked */
#define RP1_AUDIO_ERR_DMA_RST  -4   /* DMAC_RESET stuck */
#define RP1_AUDIO_ERR_DMA_MEM  -5   /* dma_alloc()/dma_phy_addr() failed */
#define RP1_AUDIO_ERR_PARAM    -6   /* bad ring geometry */
#define RP1_AUDIO_ERR_STATE    -7   /* no ring allocated / already running */

/*
 * Pack one interleaved S16_LE stereo frame into an audio_out FIFO word:
 * left in bits [15:0], right in bits [31:16]. Silence is 0 for both, which
 * is also what audio_startup() pushes into the FIFO to keep underruns from
 * producing undefined output.
 */
uint32_t rp1_audio_pack_frame(int16_t left, int16_t right);

/*
 * Bring up the RP1 register window, the audio clock tree, the audio_out
 * block and the DMAC controller, and mux GPIO12/13 to funcsel "aaud".
 * Idempotent. Must be called before anything else here.
 *
 * RP1_AUDIO_F_CHANNEL_SWAP sets AUDIO_OUT_CTRL.CHANNEL_SWAP, the hardware
 * left/right fixup. Leave it clear unless the channels come out reversed.
 */
int rp1_audio_init(uint32_t flags);

/*
 * Allocate the circular DMA ring: slots descriptors plus slots buffers of
 * slot_frames stereo frames each. Must be called while stopped. The buffers
 * are uncached sys_dma memory, so writing them needs no cache maintenance.
 * Returns 0 or RP1_AUDIO_ERR_*.
 */
int rp1_audio_setup_ring(uint32_t slots, uint32_t slot_frames);
void rp1_audio_teardown_ring(void);

/*
 * Start playback: prime the FIFO, fill every LLI, point CH_LLP at slot 0
 * and enable the channel. Returns 0 or RP1_AUDIO_ERR_*.
 */
int rp1_audio_start(void);

/*
 * Stop playback: disable the DMA channel and mute the output. The ring
 * stays allocated, so a later rp1_audio_start() can reuse it.
 */
void rp1_audio_stop(void);

bool rp1_audio_running(void);

uint32_t rp1_audio_slots(void);
uint32_t rp1_audio_slot_frames(void);

/*
 * Virtual address of a slot's sample buffer, or NULL. slot_frames 32-bit
 * words live there, one per stereo frame.
 */
uint32_t* rp1_audio_slot_buffer(uint32_t slot);

/*
 * Slot index the DMAC is working on, or -1 when it cannot be resolved
 * (channel not enabled, or CH_LLP does not point into our ring).
 */
int rp1_audio_hw_slot(void);

/*
 * True when writing slot is safe: the controller is far enough away that
 * the slot will not be fetched again for several slot periods.
 */
bool rp1_audio_slot_writable(uint32_t slot);

/*
 * Publish a refilled slot: re-arms CTL_H.LLI_VALID, which the controller
 * clears when it writes the block status back into the LLI. Without this
 * the ring dies after one lap.
 */
void rp1_audio_slot_commit(uint32_t slot);

/* Re-arm every LLI outside the guard band, for recovery after a stall. */
void rp1_audio_rearm_all(void);

#endif
