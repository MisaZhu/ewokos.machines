#ifndef BCM2712_HDMI_AUDIO_H
#define BCM2712_HDMI_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * HDMI (VC4 MAI) playback backend for BCM2712 (Raspberry Pi 5).
 *
 * Stereo audio out over HDMI0 lives in the VC4 "MAI" (Multimedia Audio
 * Interface) block of the HDMI encoder (bcm2712.dtsi hdmi0@7ef00700, the
 * "hd" register bank at 0x7c720000). It is fed by the legacy SoC DMA engine
 * ("dma32", brcm,bcm2712-dma at 0x10_00010000) on peripheral request line
 * DREQ 10 (bcm2712.dtsi: hdmi0 dmas = <&dma32 10>, dma-names = "audio-rx").
 *
 * Unlike the RP1 audio_out block (rp1_audio.h), the MAI is NOT a fixed-rate
 * PWM: it carries IEC958 subframes and can run at any of the HDMI audio
 * sample rates, so this backend is multi-rate. The caller picks a hardware
 * rate with hdmi_audio_config() and resamples anything else down to it.
 *
 * Consequences for callers:
 *
 *  - The wire format is IEC958, not raw PCM. Every 32-bit FIFO word is one
 *    IEC958 subframe: bits [27:4] hold the 24-bit sample, bit 31 is the even
 *    parity, bit 30 the channel-status bit and bits [3:0] the block preamble.
 *    Both the linux vc4_hdmi DAI (SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE) and
 *    Circle pack this in software, so we do too: hdmi_audio_iec958_subframe()
 *    builds one word, and a stereo frame is two words, LEFT then RIGHT.
 *    hdmi_audio_slot_buffer() therefore holds slot_frames * 2 words.
 *
 *  - Exactly two channels. The MAI channel mask/map and the CEA audio
 *    infoframe are programmed for stereo; HDMI0 on the Pi 5 is wired that way.
 *
 *  - Playback is a free-running circular DMA ring of bcm2835 control blocks.
 *    Once started the engine walks the ring forever (the last CB links back to
 *    the first) and never stops on its own; there is no "transfer complete".
 *    Callers must keep every slot filled (silence on underrun) and keep the
 *    ring position polled, exactly like rp1_audio.h.
 *
 * Ring model:
 *
 *   The engine is polled, not interrupt driven: EwokOS user space has no DMA
 *   IRQ here. The cyclic CB chain stays valid for the whole run, so unlike the
 *   RP1 LLI ring there is nothing to re-arm per lap; hdmi_audio_slot_commit()
 *   is only a write barrier kept for API symmetry with rp1_audio.
 *
 *   Position comes from the channel's SOURCE_AD register, which the bcm2835
 *   engine keeps pointed at the byte it is about to read; linux reads the same
 *   register back in bcm2835_dma_tx_status() for a mem-to-dev transfer.
 */

#define HDMI_AUDIO_CHANNELS     2u
/* 24-bit samples carried in 32-bit IEC958 subframes */
#define HDMI_AUDIO_BITS         24u
/* two IEC958 subframe words per stereo frame: LEFT then RIGHT */
#define HDMI_AUDIO_FRAME_WORDS  2u

/*
 * Native hardware sample rates. These are exactly the ones the linux vc4_hdmi
 * DAI advertises (SNDRV_PCM_RATE_32000 .. _192000); each has a well-defined
 * MAI format code and a clock-regeneration N/CTS pair. A caller rate that is
 * not in this list is resampled to hdmi_audio_nearest_rate() first.
 */
extern const uint32_t hdmi_audio_native_rates[];
extern const uint32_t hdmi_audio_native_rate_count;

/*
 * Default ring geometry: 16 slots of 512 stereo frames = 4 KB per slot. At
 * 48 kHz that is 10.67 ms per slot and ~170 ms of buffered audio, the same
 * order the RP1 backend uses, which keeps the feeder cadence unchanged. At
 * 192 kHz it is 2.67 ms per slot, still ~11 writable slots of headroom.
 */
#define HDMI_AUDIO_DEF_SLOTS        16u
#define HDMI_AUDIO_DEF_SLOT_FRAMES  512u

/*
 * Guard band around the slot the engine is working on. SOURCE_AD is read
 * without being able to tell whether the engine has already latched the next
 * CB, so the slots on both sides of the reported one are treated as in flight.
 */
#define HDMI_AUDIO_GUARD_SLOTS      2u

/* init flags (reserved; kept for symmetry with rp1_audio_init) */
#define HDMI_AUDIO_F_NONE           0u

/* error codes returned by hdmi_audio_init()/config()/start() */
#define HDMI_AUDIO_ERR_NONE         0
#define HDMI_AUDIO_ERR_MAP         -1   /* SYS_MEM_MAP refused a window */
#define HDMI_AUDIO_ERR_CLOCK       -2   /* firmware pixel clock unavailable */
#define HDMI_AUDIO_ERR_DMA_RST     -3   /* dma32 channel would not reset */
#define HDMI_AUDIO_ERR_DMA_MEM     -4   /* dma_alloc()/dma_phy_addr() failed */
#define HDMI_AUDIO_ERR_PARAM       -5   /* bad ring geometry / rate */
#define HDMI_AUDIO_ERR_STATE       -6   /* not ready / no ring / already running */
#define HDMI_AUDIO_ERR_NO_DISPLAY  -7   /* HDMI RAM packet engine not enabled */

/*
 * Build one IEC958 subframe word from a signed 24-bit sample (already
 * shifted into bits [23:0]; only the low 24 bits are used). subframe_idx is
 * the position within the 192-frame IEC958 block counted in subframes
 * (0..383, LEFT then RIGHT), so the block start is subframe_idx < 2: both
 * subframes of frame 0 carry the "B" preamble (0x8), the value the MAI's
 * B_FRAME_IDENTIFIER is programmed to match. Basic PCM leaves the validity,
 * user and channel-status bits clear and sets the even-parity bit (31).
 */
uint32_t hdmi_audio_iec958_subframe(int32_t sample24, uint32_t subframe_idx);

/*
 * Bring up the HDMI register window (inside the main MMIO block) and the
 * dma32 window, and reset the MAI. Idempotent. Must be called first.
 *
 * This does NOT require the display to be up yet; the pixel clock is only
 * needed by hdmi_audio_start(). Returns 0 or HDMI_AUDIO_ERR_*.
 */
int hdmi_audio_init(uint32_t flags);

/*
 * Select the hardware sample rate. rate must be one of hdmi_audio_native_rates
 * (use hdmi_audio_nearest_rate() to snap an arbitrary rate). Must be called
 * while stopped and before hdmi_audio_start(); it is what the driver programs
 * into MAI_FORMAT / MAI_SMP / the clock-regeneration N and CTS. Returns 0 or
 * HDMI_AUDIO_ERR_*.
 */
int hdmi_audio_config(uint32_t rate, uint32_t channels);

/* The rate last accepted by hdmi_audio_config(), or 0 when unset. */
uint32_t hdmi_audio_rate(void);

/* True when rate is one of the native hardware rates. */
bool hdmi_audio_rate_supported(uint32_t rate);

/*
 * The native rate closest to rate (ties round up to the higher rate). Always
 * returns a member of hdmi_audio_native_rates.
 */
uint32_t hdmi_audio_nearest_rate(uint32_t rate);

/*
 * Allocate the circular DMA ring: slots control blocks plus slots sample
 * buffers of slot_frames stereo frames each (slot_frames * 2 words). Must be
 * called while stopped. The buffers are uncached sys_dma memory, so writing
 * them needs no cache maintenance. Returns 0 or HDMI_AUDIO_ERR_*.
 */
int hdmi_audio_setup_ring(uint32_t slots, uint32_t slot_frames);
void hdmi_audio_teardown_ring(void);

/*
 * Start playback: read the current pixel clock, program the MAI (format,
 * clock regeneration, channel map, thresholds) and the CEA audio infoframe,
 * prime the CB chain and enable the dma32 channel, then enable the MAI.
 * Returns 0 or HDMI_AUDIO_ERR_*.
 */
int hdmi_audio_start(void);

/*
 * Stop playback: abort the dma32 channel and mute the MAI. The ring stays
 * allocated, so a later hdmi_audio_start() can reuse it.
 */
void hdmi_audio_stop(void);

bool hdmi_audio_running(void);

uint32_t hdmi_audio_slots(void);
uint32_t hdmi_audio_slot_frames(void);

/*
 * Virtual address of a slot's sample buffer, or NULL. slot_frames * 2 32-bit
 * IEC958 subframe words live there: LEFT then RIGHT for each stereo frame.
 */
uint32_t* hdmi_audio_slot_buffer(uint32_t slot);

/*
 * Slot index the dma32 engine is working on, or -1 when it cannot be resolved
 * (channel not active, or SOURCE_AD does not point into our ring).
 */
int hdmi_audio_hw_slot(void);

/*
 * True when writing slot is safe: the engine is far enough away that the slot
 * will not be fetched again for several slot periods.
 */
bool hdmi_audio_slot_writable(uint32_t slot);

/*
 * Publish a refilled slot. The cyclic CB chain needs no re-arm, so this is
 * only a store barrier that orders the sample writes before the next poll of
 * the engine position. Kept for API symmetry with rp1_audio_slot_commit().
 */
void hdmi_audio_slot_commit(uint32_t slot);

#endif
