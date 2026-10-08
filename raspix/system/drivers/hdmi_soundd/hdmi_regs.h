#ifndef HDMI_SOUND_REGS_H
#define HDMI_SOUND_REGS_H

/*
 * VC4 HDMI audio ("MAI" = Master Audio Interface) register map.
 *
 * The layout is taken straight from the Linux vc4 HDMI driver
 * (drivers/gpu/drm/vc4/vc4_hdmi_regs.h + vc4_regs.h) so the audio
 * programming sequence matches the proven upstream one.
 *
 * The registers live in two (bcm2835) or three (bcm2711) physical blocks:
 *   - VC4_HD    : the audio/DMA block (MAI_CTL/THR/FMT/DATA/SMP, M_CTL,
 *                 FRAME_COUNT). This is the block the audio DMA writes to.
 *   - VC4_HDMI  : the HDMI core block (channel map, MAI config, audio
 *                 packet config, N/CTS, timing, packet RAM enable).
 *   - VC5_RAM   : only on bcm2711, the infoframe packet RAM window.
 *
 * Offsets below are relative to each block base. The board table turns
 * them into _mmio_base-relative offsets and the DMA-visible VC bus
 * address of MAI_DATA.
 */

#include <stdint.h>
#include <stdbool.h>

/* ---- VC4_HD (audio) block bits -------------------------------------- */
#define VC4_HD_M_ENABLE                 (1u << 0)

#define VC4_HD_MAI_CTL_DLATE            (1u << 15)
#define VC4_HD_MAI_CTL_BUSY             (1u << 14)
#define VC4_HD_MAI_CTL_CHALIGN          (1u << 13)
#define VC4_HD_MAI_CTL_WHOLSMP          (1u << 12)
#define VC4_HD_MAI_CTL_FULL             (1u << 11)
#define VC4_HD_MAI_CTL_EMPTY            (1u << 10)
#define VC4_HD_MAI_CTL_FLUSH            (1u << 9)
#define VC4_HD_MAI_CTL_PAREN            (1u << 8)
#define VC4_HD_MAI_CTL_CHNUM_SHIFT      4
#define VC4_HD_MAI_CTL_CHNUM_MASK       (0xfu << 4)
#define VC4_HD_MAI_CTL_ENABLE           (1u << 3)
#define VC4_HD_MAI_CTL_ERRORE           (1u << 2)  /* underflow, w1c */
#define VC4_HD_MAI_CTL_ERRORF           (1u << 1)  /* overflow, w1c */
#define VC4_HD_MAI_CTL_RESET            (1u << 0)

/* MAI_SMP: sampling period converges to N/(M+1) HSM cycles. */
#define VC4_HD_MAI_SMP_N_SHIFT          8
#define VC4_HD_MAI_SMP_N_MASK           (0xffffffu << 8)
#define VC4_HD_MAI_SMP_M_SHIFT          0
#define VC4_HD_MAI_SMP_M_MASK           (0xffu)

/* ---- VC4_HDMI (core) block bits ------------------------------------- */
#define VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE  (1u << 27)
#define VC4_HDMI_MAI_CONFIG_BIT_REVERSE     (1u << 26)
#define VC4_HDMI_MAI_CHANNEL_MASK_SHIFT     0
#define VC4_HDMI_MAI_CHANNEL_MASK_MASK      (0xffffu)

#define VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT      (1u << 29)
#define VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS (1u << 24)
#define VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER_SHIFT      10
#define VC4_HDMI_AUDIO_PACKET_CEA_MASK_SHIFT                0
#define VC4_HDMI_AUDIO_PACKET_CEA_MASK_MASK                 (0xffu)

#define VC4_HDMI_MAI_FORMAT_AUDIO_FORMAT_SHIFT  16
#define VC4_HDMI_MAI_FORMAT_SAMPLE_RATE_SHIFT   8
#define VC4_HDMI_MAI_FORMAT_PCM                 2u
#define VC4_HDMI_MAI_FORMAT_HBR                 200u

/* MAI sample-rate codes for HDMI_MAI_FMT. */
#define VC4_HDMI_MAI_SAMPLE_RATE_NOT_INDICATED  0u
#define VC4_HDMI_MAI_SAMPLE_RATE_8000           1u
#define VC4_HDMI_MAI_SAMPLE_RATE_11025          2u
#define VC4_HDMI_MAI_SAMPLE_RATE_12000          3u
#define VC4_HDMI_MAI_SAMPLE_RATE_16000          4u
#define VC4_HDMI_MAI_SAMPLE_RATE_22050          5u
#define VC4_HDMI_MAI_SAMPLE_RATE_24000          6u
#define VC4_HDMI_MAI_SAMPLE_RATE_32000          7u
#define VC4_HDMI_MAI_SAMPLE_RATE_44100          8u
#define VC4_HDMI_MAI_SAMPLE_RATE_48000          9u
#define VC4_HDMI_MAI_SAMPLE_RATE_64000          10u
#define VC4_HDMI_MAI_SAMPLE_RATE_88200          11u
#define VC4_HDMI_MAI_SAMPLE_RATE_96000          12u
#define VC4_HDMI_MAI_SAMPLE_RATE_128000         13u
#define VC4_HDMI_MAI_SAMPLE_RATE_176400         14u
#define VC4_HDMI_MAI_SAMPLE_RATE_192000         15u

#define VC4_HDMI_RAM_PACKET_ENABLE          (1u << 16)
#define VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN    (1u << 24)
#define VC4_HDMI_CRP_CFG_N_SHIFT            0
#define VC4_HDMI_CRP_CFG_N_MASK             (0xfffffu)

#define VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE  (1u << 1)
#define VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI    (1u << 0)

#define VC4_HDMI_HOTPLUG_CONNECTED          (1u << 0)

/* Horizontal timing (HTOTAL = HAP + HFP + HSP + HBP). */
#define VC4_HDMI_HORZA_HAP_MASK             (0x1fffu << 0)
#define VC4_HDMI_HORZB_HFP_SHIFT            0
#define VC4_HDMI_HORZB_HFP_MASK             (0x3ffu << 0)
#define VC4_HDMI_HORZB_HSP_SHIFT            10
#define VC4_HDMI_HORZB_HSP_MASK             (0x3ffu << 10)
#define VC4_HDMI_HORZB_HBP_SHIFT            20
#define VC4_HDMI_HORZB_HBP_MASK             (0x3ffu << 20)

/* Vertical timing (VTOTAL = VAL + VFP + VSP + VBP). */
#define VC4_HDMI_VERTA_VAL_SHIFT            0
#define VC4_HDMI_VERTA_VAL_MASK             (0x1fffu << 0)
#define VC4_HDMI_VERTA_VFP_SHIFT            13
#define VC4_HDMI_VERTA_VFP_MASK             (0x7fu << 13)
#define VC4_HDMI_VERTA_VSP_SHIFT            20
#define VC4_HDMI_VERTA_VSP_MASK             (0x1fu << 20)
#define VC4_HDMI_VERTB_VBP_SHIFT            0
#define VC4_HDMI_VERTB_VBP_MASK             (0x1ffu << 0)

/*
 * vc5 (bcm2711) packs the horizontal/vertical timing fields differently
 * from vc4 (bcm2835): HFP moves into HORZA, HORZB keeps only HSP/HBP,
 * and VERTA uses wider VSP/VFP fields. VERTB_VBP is bits[8:0] on both.
 */
#define VC5_HDMI_HORZA_HFP_SHIFT            16
#define VC5_HDMI_HORZA_HFP_MASK             (0x1fffu << 16)
#define VC5_HDMI_HORZA_HAP_SHIFT            0
#define VC5_HDMI_HORZA_HAP_MASK             (0x3fffu << 0)
#define VC5_HDMI_HORZB_HBP_SHIFT            16
#define VC5_HDMI_HORZB_HBP_MASK             (0x7ffu << 16)
#define VC5_HDMI_HORZB_HSP_SHIFT            0
#define VC5_HDMI_HORZB_HSP_MASK             (0x7ffu << 0)
#define VC5_HDMI_VERTA_VSP_SHIFT            24
#define VC5_HDMI_VERTA_VSP_MASK             (0x1fu << 24)
#define VC5_HDMI_VERTA_VFP_SHIFT            16
#define VC5_HDMI_VERTA_VFP_MASK             (0x7fu << 16)
#define VC5_HDMI_VERTA_VAL_SHIFT            0
#define VC5_HDMI_VERTA_VAL_MASK             (0x1fffu << 0)

/* MAI FIFO / DMA thresholds. */
#define VC4_HD_MAI_THR_PANICHIGH_SHIFT      24
#define VC4_HD_MAI_THR_PANICLOW_SHIFT       16
#define VC4_HD_MAI_THR_DREQHIGH_SHIFT       8
#define VC4_HD_MAI_THR_DREQLOW_SHIFT        0
#define VC4_D0_HD_MAI_THR_PANICHIGH_SHIFT   23
#define VC4_D0_HD_MAI_THR_PANICLOW_SHIFT    15
#define VC4_D0_HD_MAI_THR_DREQHIGH_SHIFT    7
#define VC4_D0_HD_MAI_THR_DREQLOW_SHIFT     0

/* HSM clock floor used by the vc4 driver: hsm = max(120MHz, tmds * 1.01). */
#define HDMI_HSM_MIN_CLOCK_FREQ         120000000u
/* Audio infoframe: type 0x84 => RAM packet slot 4. */
#define HDMI_AUDIO_INFOFRAME_TYPE       0x84u
#define HDMI_AUDIO_INFOFRAME_PACKET_ID  (HDMI_AUDIO_INFOFRAME_TYPE - 0x80u)
#define VC4_HDMI_PACKET_STRIDE          0x24u

/*
 * Per-board register geometry. Offsets are relative to the named block
 * base; the block bases are expressed as _mmio_base-relative offsets.
 */
typedef struct {
    bool     pi4;

    /* _mmio_base-relative block offsets (ARM view of the VC bus). */
    uint32_t hd_base_off;     /* VC4_HD   audio block */
    uint32_t core_base_off;   /* VC4_HDMI core block  */
    uint32_t ram_base_off;    /* packet RAM block (core block on bcm2835) */

    /* VC-bus address the audio DMA writes MAI samples to. */
    uint32_t mai_data_bus;
    /* DMA perimap (DREQ) field value, already shifted into TI bits 16-20. */
    uint32_t dma_permap;

    /* VC4_HD block offsets. */
    uint32_t off_m_ctl;
    uint32_t off_mai_ctl;
    uint32_t off_mai_thr;
    uint32_t off_mai_fmt;
    uint32_t off_mai_data;
    uint32_t off_mai_smp;
    uint32_t off_frame_count;

    /* VC4_HDMI core block offsets. */
    uint32_t off_mai_channel_map;
    uint32_t off_mai_config;
    uint32_t off_audio_packet_config;
    uint32_t off_ram_packet_config;
    uint32_t off_ram_packet_status;
    uint32_t off_crp_cfg;
    uint32_t off_cts_0;
    uint32_t off_cts_1;
    uint32_t off_scheduler_control;
    uint32_t off_horza;
    uint32_t off_horzb;
    uint32_t off_verta0;
    uint32_t off_vertb0;
    uint32_t off_hotplug;
    uint32_t off_ram_packet_start;

    bool     mai_thr_gen5;    /* use the wider gen5 threshold fields */
    bool     channel_map_4bit;/* vc5 packs 4 bits per channel field */
} hdmi_board_t;

/* bcm2835 (Pi 3 / Pi Zero 2): HD block 0x7e808000, core 0x7e902000. */
static const hdmi_board_t HDMI_BOARD_BCM2835 = {
    .pi4             = false,
    .hd_base_off     = 0x808000u,
    .core_base_off   = 0x902000u,
    .ram_base_off    = 0x902000u,
    .mai_data_bus    = 0x7e808020u,
    .dma_permap      = 17u << 16,
    .off_m_ctl       = 0x0c,
    .off_mai_ctl     = 0x14,
    .off_mai_thr     = 0x18,
    .off_mai_fmt     = 0x1c,
    .off_mai_data    = 0x20,
    .off_mai_smp     = 0x2c,
    .off_frame_count = 0x68,
    .off_mai_channel_map     = 0x90,
    .off_mai_config          = 0x94,
    .off_audio_packet_config = 0x9c,
    .off_ram_packet_config   = 0xa0,
    .off_ram_packet_status   = 0xa4,
    .off_crp_cfg             = 0xa8,
    .off_cts_0               = 0xac,
    .off_cts_1               = 0xb0,
    .off_scheduler_control   = 0xc0,
    .off_horza               = 0xc4,
    .off_horzb               = 0xc8,
    .off_verta0              = 0xcc,
    .off_vertb0              = 0xd0,
    .off_hotplug             = 0x0c,
    .off_ram_packet_start    = 0x400,
    .mai_thr_gen5    = false,
    .channel_map_4bit= false,
};

/* bcm2711 (Pi 4) HDMI0: HD 0x7ef20000, core 0x7ef00700, packet RAM 0x7ef01b00. */
static const hdmi_board_t HDMI_BOARD_BCM2711_HDMI0 = {
    .pi4             = true,
    .hd_base_off     = 0xf20000u,
    .core_base_off   = 0xf00700u,
    .ram_base_off    = 0xf01b00u,
    .mai_data_bus    = 0x7ef2001cu,
    .dma_permap      = 10u << 16,
    .off_m_ctl       = 0x00,
    .off_mai_ctl     = 0x10,
    .off_mai_thr     = 0x14,
    .off_mai_fmt     = 0x18,
    .off_mai_data    = 0x1c,
    .off_mai_smp     = 0x20,
    .off_frame_count = 0x60,
    .off_mai_channel_map     = 0x9c,
    .off_mai_config          = 0xa0,
    .off_audio_packet_config = 0xb8,
    .off_ram_packet_config   = 0xbc,
    .off_ram_packet_status   = 0xc4,
    .off_crp_cfg             = 0xc8,
    .off_cts_0               = 0xcc,
    .off_cts_1               = 0xd0,
    .off_scheduler_control   = 0xe0,
    .off_horza               = 0xe4,
    .off_horzb               = 0xe8,
    .off_verta0              = 0xec,
    .off_vertb0              = 0xf0,
    .off_hotplug             = 0x1a8,
    .off_ram_packet_start    = 0x000,
    .mai_thr_gen5    = true,
    .channel_map_4bit= true,
};

#endif /* HDMI_SOUND_REGS_H */
