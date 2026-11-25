#ifndef GA10B_FIFO_H
#define GA10B_FIFO_H

/*
 * GPU pushbuffer / GPFIFO encoding + engine class/method ids -- pure logic,
 * no OS dependencies, unit-tested on the host (test/test_fifo.c).
 *
 * Method header layout VERIFIED against nvgpu rel-36:
 *   hw_pbdma_ga10b.h: type_inc=0x20000000 (1<<29), type_non_inc=0x60000000
 *   (3<<29); hw_pbdma_tu104.h: type_immd=0x80000000 (4<<29), type_m=7<<29.
 *   Field positions decoded from common/ce/ce_app.c nvgpu_prepare_ce_op
 *   (0x20018000 SET_OBJECT, 0x20028100 OFFSET_IN, 0x20068102 OFFSET_OUT,
 *   0x200180c0 LAUNCH_DMA -> clc7b5 methods 0x000/0x400/0x408/0x300):
 *     bits 31:29 SEC_OP | 28:16 COUNT | 15:13 SUBCHANNEL | 12:0 METHOD>>2
 *
 * GPFIFO entry VERIFIED against gm20b_pbdma_format_gpfifo_entry
 * (pbdma_gm20b_fusa.c) + pbdma_gp_entry1_length_f (mask 0x1fffff << 10):
 *   entry0 = lo32(pb_gpu_va)  (RAW, not >>2)
 *   entry1 = hi32(pb_gpu_va) | (length_in_DWORDS & 0x1fffff) << 10
 *   length is a u32 WORD count (ce_app methodSize / priv_cmdbuf e->size),
 *   NOT bytes.
 */

#include <stdint.h>

/* ---- pushbuffer method header ---- */
#define GA10B_PB_SEC_OP_INC     1u   /* incrementing: count data words follow */
#define GA10B_PB_SEC_OP_NONINC  3u   /* non-incrementing: count words, same mthd */
#define GA10B_PB_SEC_OP_IMM     4u   /* immediate data carried in the header */

/* incrementing method write: `count` data words follow, method += 4 each */
static inline uint32_t ga10b_pb_inc(uint32_t subch, uint32_t method,
                                    uint32_t count) {
    return (GA10B_PB_SEC_OP_INC << 29) | ((count & 0x1fffu) << 16) |
           ((subch & 0x7u) << 13) | ((method >> 2) & 0x1fffu);
}

static inline uint32_t ga10b_pb_noninc(uint32_t subch, uint32_t method,
                                       uint32_t count) {
    return (GA10B_PB_SEC_OP_NONINC << 29) | ((count & 0x1fffu) << 16) |
           ((subch & 0x7u) << 13) | ((method >> 2) & 0x1fffu);
}

/* single immediate-data method (no following data word).  SEC_OP=4 verified;
 * data rides in bits 28:16 (the COUNT field position). */
static inline uint32_t ga10b_pb_imm(uint32_t subch, uint32_t method,
                                    uint32_t data) {
    return (GA10B_PB_SEC_OP_IMM << 29) | ((data & 0x1fffu) << 16) |
           ((subch & 0x7u) << 13) | ((method >> 2) & 0x1fffu);
}

/* GPFIFO entry: 2 x u32; `words` = pushbuffer length in u32 words. */
static inline void ga10b_gpfifo_entry(uint32_t* e, uint64_t pb_gpu_va,
                                      uint32_t words) {
    e[0] = (uint32_t)pb_gpu_va;
    e[1] = (uint32_t)(pb_gpu_va >> 32) | ((words & 0x1fffffu) << 10);
}

/* ---- engine / channel class ids (nvgpu class.h, ga10b) ---- */
#define GA10B_CLASS_CHANNEL_GPFIFO_B  0x0000C76Fu   /* AMPERE_CHANNEL_GPFIFO_B */
#define GA10B_CLASS_DMA_COPY_B        0x0000C7B5u   /* AMPERE_DMA_COPY_B (CE) */
#define GA10B_CLASS_COMPUTE_B         0x0000C7C0u   /* AMPERE_COMPUTE_B */
#define GA10B_CLASS_GRAPHICS_B        0x0000C797u   /* AMPERE_B (gfx) */

/* generic channel method (any subchannel): data = class id */
#define GA10B_MTHD_SET_OBJECT         0x0000u
/* nvgpu emits every CE method on subchannel 4 (ce_app.c: 0x20018000
 * SET_OBJECT, 0x20028100 OFFSET_IN, 0x200180c0 LAUNCH_DMA) */
/* GR FE binds classes to fixed subchannels (3D 0, compute 1, i2m 2, 2D 3):
 * SET_OBJECT c7c0 on subch 0 traps with fe_hww_esr=0x2 info=0xc7c0 */
#define GA10B_SUBCH_COMPUTE           1u
#define GA10B_SUBCH_CE                4u

/* ---- AMPERE_DMA_COPY_B (0xC7B5) methods, verbatim clc7b5.h ---- */
#define GA10B_CE_NOP                  0x0100u
#define GA10B_CE_SET_SEMAPHORE_A      0x0240u   /* OFFSET_UPPER 16:0 */
#define GA10B_CE_SET_SEMAPHORE_B      0x0244u   /* OFFSET_LOWER 31:0 */
#define GA10B_CE_SET_SEMAPHORE_PAYLOAD 0x0248u  /* PAYLOAD 31:0 */
#define GA10B_CE_SET_SRC_PHYS_MODE    0x0260u   /* TARGET 1:0 */
#define GA10B_CE_SET_DST_PHYS_MODE    0x0264u
#define GA10B_CE_LAUNCH_DMA           0x0300u
#define GA10B_CE_OFFSET_IN_UPPER      0x0400u   /* VA 48:32 */
#define GA10B_CE_OFFSET_IN_LOWER      0x0404u   /* VA 31:0 */
#define GA10B_CE_OFFSET_OUT_UPPER     0x0408u
#define GA10B_CE_OFFSET_OUT_LOWER     0x040Cu
#define GA10B_CE_PITCH_IN             0x0410u   /* bytes per src row */
#define GA10B_CE_PITCH_OUT            0x0414u   /* bytes per dst row */
#define GA10B_CE_LINE_LENGTH_IN       0x0418u   /* bytes per line */
#define GA10B_CE_LINE_COUNT           0x041Cu   /* number of lines */

/* LAUNCH_DMA (0x300) field values (clc7b5.h) */
#define GA10B_CE_LAUNCH_NON_PIPELINED 0x00000002u   /* DATA_TRANSFER_TYPE 1:0 */
#define GA10B_CE_LAUNCH_FLUSH_ENABLE  (1u << 2)
#define GA10B_CE_LAUNCH_SEM_ONE_WORD  (1u << 3)     /* SEMAPHORE_TYPE 4:3 */
#define GA10B_CE_LAUNCH_SEM_FOUR_WORD (2u << 3)
#define GA10B_CE_LAUNCH_SRC_PITCH     (1u << 7)     /* SRC_MEMORY_LAYOUT 7:7 */
#define GA10B_CE_LAUNCH_DST_PITCH     (1u << 8)     /* DST_MEMORY_LAYOUT 8:8 */
#define GA10B_CE_LAUNCH_MULTI_LINE    (1u << 9)     /* MULTI_LINE_ENABLE 9:9 */
/* SRC_TYPE 12:12 / DST_TYPE 13:13: VIRTUAL=0 PHYSICAL=1 (default virtual) */

/* CE phys-mode TARGET (SET_SRC/DST_PHYS_MODE 1:0) */
#define GA10B_CE_TARGET_LOCAL_FB      0u
#define GA10B_CE_TARGET_SYS_COH       1u
#define GA10B_CE_TARGET_SYS_NCOH      2u

/* ---- instance block: RAMFC + RAMIN (hw_ram_ga10b.h word indices,
 * hw_pbdma_ga10b.h field values, ramfc_ga10b_fusa.c init flow) ----
 * One 4KB page: RAMFC = words 0..127 (ram_fc_size_val 0x200 bytes),
 * RAMIN starts at word 128 (byte 0x200).  Indices are u32 words. */

/* RAMFC word offsets */
#define GA10B_RAMFC_SIGNATURE       4u
#define GA10B_RAMFC_ACQUIRE         12u
#define GA10B_RAMFC_GP_BASE         18u
#define GA10B_RAMFC_GP_BASE_HI      19u
#define GA10B_RAMFC_PB_HEADER       33u
#define GA10B_RAMFC_PB_COUNT        34u
#define GA10B_RAMFC_SUBDEVICE       37u
#define GA10B_RAMFC_TARGET          43u
#define GA10B_RAMFC_HCE_CTRL        57u
#define GA10B_RAMFC_CONFIG          61u
#define GA10B_RAMFC_INTR_NOTIFY     62u
#define GA10B_RAMFC_SET_CH_INFO     63u

/* RAMFC init values */
/* gp10b_pbdma_get_signature: GPU_LIT_GPFIFO_CLASS (AMPERE_CHANNEL_GPFIFO_B)
 * | pbdma_signature_sw_zero.  0xface is hw_valid, written by HW, not SW. */
#define GA10B_RAMFC_SIG_VALID       0x0000c76fu
/* gm20b_pbdma_acquire_val(0): retry_man_2 | retry_exp_2, no timeout */
#define GA10B_RAMFC_ACQUIRE_VAL     0x00000102u
#define GA10B_RAMFC_PB_HEADER_VAL   0x20400000u  /* type_inc | first */
#define GA10B_RAMFC_SUBDEVICE_VAL   0x30000001u  /* id1|active|dma_enable */
#define GA10B_RAMFC_TARGET_CTX_VALID 0x00030000u /* eng_ctx|ce_ctx valid */
#define GA10B_RAMFC_HCE_PRIV        0x00000020u
#define GA10B_RAMFC_CONFIG_VAL      0x00001100u  /* auth_priv|userd_writeback */
#define GA10B_RAMFC_INTR_CPU_ENABLE 0x80000000u  /* gsp_disable = 0 */

/* gp_base: offset field holds (gpu_va>>3) in bits 31:3 */
static inline uint32_t ga10b_gp_base(uint64_t gpfifo_gpu_va) {
    return (uint32_t)(((gpfifo_gpu_va >> 3) & 0x1fffffffu) << 3);
}
/* gp_base_hi: va bits 39:32 | log2(entry_count) in bits 20:16 */
static inline uint32_t ga10b_gp_base_hi(uint64_t gpfifo_gpu_va,
                                        uint32_t log2_entries) {
    return (uint32_t)((gpfifo_gpu_va >> 32) & 0xffu) |
           ((log2_entries & 0x1fu) << 16);
}
/* target: rleng_id bits 1:0 | eng/ce ctx valid */
static inline uint32_t ga10b_ramfc_target(uint32_t rleng_id) {
    return (rleng_id & 0x3u) | GA10B_RAMFC_TARGET_CTX_VALID;
}
/* intr_notify: vector bits 11:0 | cpu_enable */
static inline uint32_t ga10b_ramfc_intr_notify(uint32_t vector) {
    return (vector & 0xfffu) | GA10B_RAMFC_INTR_CPU_ENABLE;
}
/* set_channel_info: veid bits 13:8 | chid bits 27:16 */
static inline uint32_t ga10b_ramfc_ch_info(uint32_t veid, uint32_t chid) {
    return ((veid & 0x3fu) << 8) | ((chid & 0xfffu) << 16);
}

/* RAMIN word offsets (page_dir_base_lo_w=128 etc.) */
#define GA10B_RAMIN_PDB_LO          128u
#define GA10B_RAMIN_PDB_HI          129u
#define GA10B_RAMIN_FW_MAGIC        131u
#define GA10B_RAMIN_WFI_VEID        134u
#define GA10B_RAMIN_FW_MAGIC_VAL    0xcafeca11u
/* engine method buffer (runqueue) GPU VA, raw unshifted (gv11b ramin).
 * PBDMA posts fetched methods here for the engine; without a valid
 * zeroed runqueue the channel fetches but never executes. */
#define GA10B_RAMIN_ENG_MTHD_BUF_LO 136u
#define GA10B_RAMIN_ENG_MTHD_BUF_HI 137u

/* pdb_lo: target ncoh(3)/coh(2) | vol(4) | ver2(0x400) | big64K(0x800) |
 * (pdb_pa>>12)&0xfffff in bits 31:12.  target selects the sysmem aperture
 * (hw_ram_ga10b.h ram_in_page_dir_base_target_sys_mem_{coh,ncoh}_f). */
static inline uint32_t ga10b_ramin_pdb_lo_ap(uint64_t pdb_phy, uint32_t target) {
    return target | 0x4u | 0x400u | 0x800u |
           (uint32_t)(((pdb_phy >> 12) & 0xfffffu) << 12);
}
static inline uint32_t ga10b_ramin_pdb_lo(uint64_t pdb_phy) {
    return ga10b_ramin_pdb_lo_ap(pdb_phy, 0x3u);   /* sys_ncoh */
}

/* ---- USERD (256B in its own page; hw_ram_ga10b.h ram_userd_*_w) ---- */
#define GA10B_USERD_PB_PUT          16u
#define GA10B_USERD_PB_GET          17u
#define GA10B_USERD_GP_GET          34u
#define GA10B_USERD_GP_PUT          35u

/* ---- runlist buffer entries, 16B each (hw_ram_ga10b.h ram_rl_entry_*)
 * entry_type: channel=0, tsg=1.  TSG must precede its channels. */

/* TSG entry (gv11b_runlist_get_tsg_entry): the timeslice must be non-zero.
 * Default NVGPU_TSG_TIMESLICE_DEFAULT_US 1024 at the 31.25 MHz ptimer
 * reference -> timeout 128 (bits 31:24), scale 3 (bits 19:16). */
#define GA10B_RL_TSG_TIMESLICE      ((128u << 24) | (3u << 16))
static inline void ga10b_rl_tsg(uint32_t* e, uint32_t tsgid, uint32_t nch) {
    e[0] = 1u | GA10B_RL_TSG_TIMESLICE; /* entry_type_tsg */
    e[1] = nch & 0xffu;                 /* tsg_length */
    e[2] = tsgid & 0xfffu;
    e[3] = 0;
}

/* channel entry: inst/userd physical pointers, sys_ncoh aperture */
static inline void ga10b_rl_ch(uint32_t* e, uint32_t chid,
                               uint64_t inst_phy, uint64_t userd_phy) {
    e[0] = (3u << 4) |                  /* inst_target sys_ncoh */
           (3u << 6) |                  /* userd_target sys_ncoh */
           (uint32_t)(((userd_phy >> 8) & 0xffffffu) << 8);
    e[1] = (uint32_t)(userd_phy >> 32);
    e[2] = (chid & 0xfffu) |
           (uint32_t)(((inst_phy >> 12) & 0xfffffu) << 12);
    e[3] = (uint32_t)(inst_phy >> 32);
}

/* ---- AMPERE_COMPUTE_B (0xC7C0) methods, verbatim clc7c0.h ---- */
#define GA10B_CP_SET_QMD_VERSION      0x0288u   /* current 15:0 | oldest 31:16 */
#define GA10B_CP_INVALIDATE_SKED_CACHES 0x0298u
#define GA10B_CP_SET_CWD_SLOT_COUNT   0x02B0u
#define GA10B_CP_SEND_PCAS_A          0x02B4u   /* qmd_gpu_va >> 8 */
#define GA10B_CP_SEND_SIGNALING_PCAS2_B 0x02C0u /* action 3:0 */
#define GA10B_CP_PCAS2_INV_COPY_SCHED 0x3u
#define GA10B_CP_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A 0x02E4u /* +B +C */
#define GA10B_CP_SET_INLINE_QMD_ADDRESS_A 0x0318u
#define GA10B_CP_SET_INLINE_QMD_ADDRESS_B 0x031Cu
#define GA10B_CP_LOAD_INLINE_QMD_DATA(i) (0x0320u + 4u * (i))
#define GA10B_CP_SET_SHADER_LOCAL_MEMORY_A 0x0790u   /* upper 16:0, +B lower */
#define GA10B_CP_SET_SHADER_LOCAL_MEMORY_WINDOW_A 0x07B0u  /* +B */
#define GA10B_CP_SET_SHADER_SHARED_MEMORY_WINDOW_A 0x02A0u /* +B */
/* QMDV03_00 version nibble: major 3 << 4 | release 0 (clc7c0qmd.h dw18) */
#define GA10B_QMD_VERSION_V03_00      0x00300030u

/* Minimal QMDV03_00 (64 dwords, caller zeroes): 1x1x1 grid, 32x1x1
 * threads, RELEASE0 one-word semaphore with sysmembar, sm87 SASS.
 * Field positions verbatim clc7c0qmd.h MW() ranges / 32.
 * reg_count is the RAW per-thread GPR count, written directly into
 * REGISTER_COUNT_V (bits 656:648 = dword20[16:8]).  NAK qmd.rs does
 * set_register_count(info.num_gprs) with no rounding -- the /8 register
 * allocation granularity is the SM's job, not this field's. */
static inline void ga10b_qmd_minimal(uint32_t* q, uint64_t program_gpu_va,
                                     uint64_t sem_gpu_va, uint32_t payload,
                                     uint32_t reg_count) {
    q[4]  = 1u << 6;                    /* SM_GLOBAL_CACHING_ENABLE */
    q[5]  = 0xfc000000u;                /* invalidate tex/shader/const caches */
    q[12] = 1;                          /* CTA_RASTER_WIDTH */
    q[13] = 1;                          /* CTA_RASTER_HEIGHT */
    q[14] = 1;                          /* CTA_RASTER_DEPTH */
    q[18] = 0x30u | (32u << 16);        /* version V03_00 | THREAD_DIM0=32 */
    q[19] = 1u | (1u << 16);            /* THREAD_DIM1=1 | DIM2=1 */
    q[20] = (reg_count & 0x1ffu) << 8;  /* REGISTER_COUNT_V = raw GPR count */
    q[24] = (uint32_t)sem_gpu_va;       /* RELEASE0_ADDRESS_LOWER */
    q[25] = (uint32_t)((sem_gpu_va >> 32) & 0xffu) |
            (1u << 19) |                /* RELEASE0_MEMBAR_TYPE FE_SYSMEMBAR */
            (1u << 23) |                /* RELEASE0_ENABLE */
            (1u << 30);                 /* STRUCT_SIZE ONE_WORD */
    q[26] = payload;                    /* RELEASE0_PAYLOAD_LOWER */
    q[48] = (uint32_t)program_gpu_va;   /* PROGRAM_ADDRESS_LOWER */
    q[49] = (uint32_t)((program_gpu_va >> 32) & 0x1ffffu);
    q[51] = 0x87u << 24;                /* SASS_VERSION sm87 */
}

#endif