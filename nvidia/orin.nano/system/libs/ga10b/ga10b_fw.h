#ifndef GA10B_FW_H
#define GA10B_FW_H

/*
 * GA10B firmware descriptor layouts -- transcribed from OE4T linux-nvgpu
 * rel-36 (MIT core):
 *   common/acr/nvgpu_acr_interface.h   (lsf_wpr_header, lsf_lsb_header,
 *                                       flcn_bl_dmem_desc, flcn2_acr_desc)
 *   include/nvgpu/pmu.h                (falcon_next_core_ucode_desc)
 *   include/nvgpu/falcon.h             (FALCON_ID_*)
 *   common/acr/acr_blob_construct_v0.h (blob alignment constants)
 *
 * Pure parsing/validation lives in ga10b_fw_parse.c (no OS deps, host
 * unit-testable against the real blobs).  DMA/file plumbing is in
 * ga10b_fw.c.
 *
 * The blobs themselves are NVIDIA proprietary (Tegra_Software_License),
 * extracted by the user from linux-firmware-nvidia-tegra / L4T
 * nvidia-l4t-firmware.  They are NOT in this repo.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- falcon ids (nvgpu falcon.h) ---- */
#define GA10B_FALCON_PMU            0u
#define GA10B_FALCON_GSPLITE        1u   /* ACR bootstrap owner on ga10b */
#define GA10B_FALCON_FECS           2u
#define GA10B_FALCON_GPCCS          3u
#define GA10B_FALCON_PMU_NEXT_CORE  13u
#define GA10B_FALCON_INVALID        0xffffffffu

/* ---- LS blob alignment (acr_blob_construct_v0.h) ---- */
#define GA10B_LSF_WPR_HDR_ALIGN     256u
#define GA10B_LSF_LSB_HDR_ALIGN     256u
#define GA10B_LSF_BL_DATA_ALIGN     256u
#define GA10B_LSF_UCODE_DATA_ALIGN  4096u

/* ---- ACR HS (GSP RISC-V) firmware file names, prod non-safety set
 * (acr_sw_ga10b.c GSPPROD_RISCV_ACR_FW_*) ---- */
#define GA10B_FW_ACR_MANIFEST "acr-gsp.manifest.encrypt.bin.out.bin.prod"
#define GA10B_FW_ACR_TEXT     "acr-gsp.text.encrypt.bin.prod"
#define GA10B_FW_ACR_DATA     "acr-gsp.data.encrypt.bin.prod"

/* ---- PMU next-core (RISC-V) RTOS firmware (common/pmu/fw/fw.c) ---- */
#define GA10B_FW_PMU_DESC     "gpmu_ucode_next_prod_desc.bin"
#define GA10B_FW_PMU_IMAGE    "gpmu_ucode_next_prod_image.bin"
#define GA10B_FW_PMU_SIG      "pmu_pkc_prod_sig.bin"

/* ---- FECS/GPCCS ctxsw firmware.  The 272B fecs/gpccs_encrypt_prod.bin
 * file = nvgpu_ctxsw_bootloader_desc (16B) + 256B encrypted boot image.
 * The app code/data bodies come from the NETC netlist regions (plain
 * u32 lists), assembled into a boot+code+data surface by the driver
 * (gr_falcon.c nvgpu_gr_falcon_init_ctxsw_ucode).  ACR decrypts the
 * whole LS ucode into WPR. ---- */
#define GA10B_FW_FECS_ENC     "fecs_encrypt_prod.bin"
#define GA10B_FW_FECS_SIG     "fecs_pkc_sig_encrypt.bin"
#define GA10B_FW_GPCCS_ENC    "gpccs_encrypt_prod.bin"
#define GA10B_FW_GPCCS_SIG    "gpccs_pkc_sig_encrypt.bin"

/* ---- GR netlist images (hal/netlist) ---- */
#define GA10B_FW_NET_A        "NETA_img_prod_encrypted.bin"
#define GA10B_FW_NET_B        "NETB_img_prod_encrypted.bin"
#define GA10B_FW_NET_C        "NETC_img_prod_encrypted.bin"
#define GA10B_FW_NET_D        "NETD_img_prod_encrypted.bin"

/* PKC signature blob: 384B sig padded to 512 per component (code+data),
 * plus a 2048B public key (nvgpu_acr_interface.h PKC_* constants).
 * Observed prod sig files are 2248B = 200B header + 2048B key. */
#define GA10B_PKC_SIG_SIZE          384u
#define GA10B_PKC_SIG_PADDED_SIZE   512u
#define GA10B_PKC_PK_SIZE           2048u
#define GA10B_PKC_SIG_FILE_SIZE     2248u
#define GA10B_PKC_SIG_HDR_SIZE      200u

/* ---- PMU next-core ucode descriptor: the 76-byte
 * gpmu_ucode_next_prod_desc.bin file IS this struct (19 x u32; the
 * trailing bool is transmitted as a full u32). ---- */
typedef struct {
    uint32_t version;
    uint32_t bootloader_offset;
    uint32_t bootloader_size;
    uint32_t bootloader_param_offset;
    uint32_t bootloader_param_size;
    uint32_t next_core_elf_offset;
    uint32_t next_core_elf_size;
    uint32_t app_version;
    uint32_t manifest_offset;
    uint32_t manifest_size;
    uint32_t monitor_data_offset;
    uint32_t monitor_data_size;
    uint32_t monitor_code_offset;
    uint32_t monitor_code_size;
    uint32_t is_monitor_enabled;
    uint32_t pad[4];
} ga10b_pmu_ucode_desc_t;

/* ---- LS falcon WPR header (one per managed LS falcon in the non-WPR
 * blob, terminated by an entry with falcon_id = INVALID) ---- */
typedef struct {
    uint32_t falcon_id;
    uint32_t lsb_offset;        /* from non-WPR blob base */
    uint32_t bootstrap_owner;
    uint32_t lazy_bootstrap;    /* 1 = LS-PMU bootstraps, ACR skips */
    uint32_t bin_version;
    uint32_t status;            /* written back by ACR HS */
} ga10b_lsf_wpr_header_t;

/* ---- LS falcon signature descriptor (v1, pre-PKC layout kept for
 * reference; ga10b uses PKC v2 signatures carried in the sig files) ---- */
typedef struct {
    uint8_t  prd_keys[2][16];
    uint8_t  dbg_keys[2][16];
    uint32_t b_prd_present;
    uint32_t b_dbg_present;
    uint32_t falcon_id;
    uint32_t bsupports_versioning;
    uint32_t version;
    uint32_t dep_map_count;
    uint8_t  dep_map[11 * 2 * 4];
    uint8_t  kdf[16];
} ga10b_lsf_ucode_desc_t;

/* ---- LS falcon LSB header ---- */
typedef struct {
    ga10b_lsf_ucode_desc_t signature;
    uint32_t ucode_off;         /* from non-WPR blob base */
    uint32_t ucode_size;
    uint32_t data_size;
    uint32_t bl_code_size;
    uint32_t bl_imem_off;
    uint32_t bl_data_off;       /* from non-WPR blob base */
    uint32_t bl_data_size;
    uint32_t app_code_off;
    uint32_t app_code_size;
    uint32_t app_data_off;
    uint32_t app_data_size;
    uint32_t flags;
} ga10b_lsf_lsb_header_t;

/* LS blob flags (nvgpu_acr_interface.h NV_FLCN_ACR_LSF_FLAG_*) */
#define GA10B_LSF_FLAG_LOAD_CODE_AT_0    (1u << 0)
#define GA10B_LSF_FLAG_DMACTL_REQ_CTX    (1u << 2)
#define GA10B_LSF_FLAG_FORCE_PRIV_LOAD   (1u << 3)

/* ---- falcon bootloader DMEM descriptor (filled by the driver, copied
 * into the blob at bl_data_off; the BL uses it to DMA the LS ucode).
 * nvgpu uses struct falc_u64 {u32 lo,hi} for the DMA bases -- NOT a
 * native u64 -- so the on-wire layout has no 8-byte padding (84B). ---- */
typedef struct {
    uint32_t lo, hi;
} ga10b_falc_u64_t;

typedef struct {
    uint32_t reserved[4];
    uint32_t signature[4];
    uint32_t ctx_dma;           /* DMA index (GK20A_PMU_DMAIDX_UCODE) */
    ga10b_falc_u64_t code_dma_base;  /* 256B-aligned physical */
    uint32_t non_sec_code_off;
    uint32_t non_sec_code_size;
    uint32_t sec_code_off;
    uint32_t sec_code_size;
    uint32_t code_entry_point;
    ga10b_falc_u64_t data_dma_base;  /* 256B-aligned physical */
    uint32_t data_size;         /* multiple of 256 */
    uint32_t argc;
    uint32_t argv;
} ga10b_flcn_bl_dmem_desc_t;

/* ---- WPR region descriptors handed to ACR HS ---- */
typedef struct {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t region_id;
    uint32_t read_mask;
    uint32_t write_mask;
    uint32_t client_mask;
    uint32_t shadow_mem_start_addr;
} ga10b_flcn_acr_region_prop_t;

typedef struct {
    uint32_t no_regions;
    ga10b_flcn_acr_region_prop_t region_props[2];
} ga10b_flcn_acr_regions_t;

/* ---- flcn2_acr_desc: the non-WPR sysmem descriptor ACR HS reads to
 * find the LS blob + WPR config (ga10b non-FuSa path,
 * acr_sw_ga10b.c ga10b_acr_patch_wpr_info_to_ucode).
 * On ga10b wpr_region_id=0 / no_regions=1 / region props zeroed => ACR
 * HS fetches the WPR1 bounds from hardware (locked by MB1/MB2/BPMP).
 * Layout matches nvgpu exactly: blob_start u64 lands at offset 72 with
 * no explicit padding (sizeof = 96). ---- */
typedef struct {
    uint32_t wpr_region_id;
    uint32_t wpr_offset;
    ga10b_flcn_acr_regions_t regions;
    uint32_t nonwpr_ucode_blob_size;
    uint64_t nonwpr_ucode_blob_start;
    uint64_t ls_pmu_desc;       /* phys addr of PMU next-core desc copy */
    uint32_t gpu_mode;
} ga10b_flcn2_acr_desc_t;

/* ---- FECS/GPCCS encrypted-ucode header: the 272-byte
 * fecs_encrypt_prod.bin / gpccs_encrypt_prod.bin files = this 16B desc
 * followed by a 256B boot image (gr_falcon.c parses the desc). ---- */
#define GA10B_CTXSW_ENC_HDR_SIZE  272u

typedef struct {
    uint32_t start_offset;      /* unused (0) */
    uint32_t size;              /* boot image bytes (256) */
    uint32_t imem_offset;       /* FECS 0x9e00, GPCCS 0x3800 */
    uint32_t entry_point;       /* == imem_offset */
} ga10b_ctxsw_bootloader_desc_t;

/* ---- PKC v2 LS signature descriptor (nvgpu_acr_interface.h).  The
 * 2248B sig file IS the wrapper below, copied verbatim into the LSB
 * header with only falcon_id patched. ---- */
#define GA10B_LSF_SIG_PKC_PADDED  512u   /* 384B sig + 128B pad, per component */
#define GA10B_LSF_DEPMAP_BYTES    88u    /* 11 * 2 * 4 */

typedef struct {
    uint16_t identifier;        /* 1 */
    uint16_t version;           /* 2 */
    uint32_t size;              /* 2248 */
} ga10b_acr_generic_hdr_t;

typedef struct {
    uint32_t falcon_id;
    uint8_t  b_prd_present;
    uint8_t  b_dbg_present;
    uint16_t reserved;
    uint32_t sig_size;
    uint8_t  prod_sig[2][GA10B_LSF_SIG_PKC_PADDED];
    uint8_t  debug_sig[2][GA10B_LSF_SIG_PKC_PADDED];
    uint16_t sig_algo_ver;
    uint16_t sig_algo;
    uint16_t hash_algo_ver;
    uint16_t hash_algo;
    uint32_t sig_algo_padding_type;
    uint8_t  dep_map[GA10B_LSF_DEPMAP_BYTES];
    uint32_t dep_map_count;
    uint8_t  b_supports_versioning;
    uint8_t  pad[3];
    uint32_t ls_ucode_version;
    uint32_t ls_ucode_id;
    uint32_t b_ucode_ls_encrypted;
    uint32_t ls_encalgo_type;
    uint32_t ls_enc_algo_ver;
    uint8_t  ls_enc_iv[16];
    uint8_t  rsvd[36];
} ga10b_lsf_ucode_desc_v2_t;      /* 2240B */

typedef struct {
    ga10b_acr_generic_hdr_t generic_hdr;
    ga10b_lsf_ucode_desc_v2_t v2;
} ga10b_lsf_ucode_desc_wrapper_t; /* 2248B == sig file */

/* ---- LS falcon LSB header v1 (PKC): signature wrapper + 12 u32. ---- */
typedef struct {
    ga10b_lsf_ucode_desc_wrapper_t signature;
    uint32_t ucode_off;         /* from non-WPR blob base */
    uint32_t ucode_size;
    uint32_t data_size;
    uint32_t bl_code_size;
    uint32_t bl_imem_off;
    uint32_t bl_data_off;       /* from non-WPR blob base */
    uint32_t bl_data_size;
    uint32_t app_code_off;
    uint32_t app_code_size;
    uint32_t app_data_off;
    uint32_t app_data_size;
    uint32_t flags;
} ga10b_lsf_lsb_header_v1_t;    /* 2296B */

/* WPR header status written by the driver before ACR runs */
#define GA10B_LSF_IMAGE_STATUS_COPY  1u
/* extra WPR reserved after a PMU next-core (RISC-V) image */
#define GA10B_PMU_WPR_RSVD           0x8000u

/* ---- netlist region ids carrying the ctxsw ucode bodies (NETC) ---- */
#define GA10B_NET_FECS_DATA   0u
#define GA10B_NET_FECS_INST   1u
#define GA10B_NET_GPCCS_DATA  2u
#define GA10B_NET_GPCCS_INST  3u

/* ---- driver-side per-falcon image metadata (the fields of nvgpu
 * ls_falcon_ucode_desc that the blob builder consumes; not on-wire). ---- */
typedef struct {
    uint32_t bootloader_start_offset;
    uint32_t bootloader_size;        /* ALIGN(boot,256) */
    uint32_t bootloader_imem_offset;
    uint32_t bootloader_entry_point;
    uint32_t image_size;             /* ALIGN(boot)+ALIGN(code)+ALIGN(data) */
    uint32_t app_size;               /* ALIGN(code)+ALIGN(data) */
    uint32_t app_start_offset;       /* = bootloader_size */
    uint32_t app_imem_offset;        /* 0 */
    uint32_t app_imem_entry;         /* 0 */
    uint32_t app_dmem_offset;        /* 0 */
    uint32_t app_resident_code_offset; /* 0 */
    uint32_t app_resident_code_size;
    uint32_t app_resident_data_offset;
    uint32_t app_resident_data_size;
} ga10b_ls_image_desc_t;

/* one managed LS falcon image fed to the pure blob builder */
typedef struct {
    uint32_t falcon_id;
    uint32_t lazy_bootstrap;
    uint32_t is_priv_load;
    uint32_t is_next_core;       /* PMU RISC-V: full image, no BL desc */
    ga10b_ls_image_desc_t desc;  /* unused when is_next_core */
    const void* data;            /* surface bytes (or full PMU image) */
    uint32_t data_size;          /* bytes copied at ucode_off */
    const ga10b_lsf_ucode_desc_wrapper_t* sig;  /* falcon_id pre-patched */
    const ga10b_pmu_ucode_desc_t* ndesc;        /* next-core only */
} ga10b_ls_image_t;

/* ---- pure parse/validate (ga10b_fw_parse.c) ---- */
int ga10b_fw_parse_pmu_desc(const void* buf, uint32_t size,
                            uint32_t image_size, ga10b_pmu_ucode_desc_t* out);
int ga10b_fw_check_pkc_sig(const void* buf, uint32_t size,
                           uint32_t falcon_id);
int ga10b_fw_check_ctxsw_enc(const void* buf, uint32_t size);
int ga10b_fw_check_acr_set(uint32_t manifest_sz, uint32_t text_sz,
                           uint32_t data_sz);
/* build the flcn2_acr_desc for a constructed LS blob */
void ga10b_fw_fill_acr_desc(ga10b_flcn2_acr_desc_t* d,
                            uint64_t blob_phy, uint32_t blob_size,
                            uint64_t pmu_desc_phy);
/* parse the 16B ctxsw boot desc at the head of a 272B enc file */
int ga10b_fw_parse_ctxsw_boot(const void* buf, uint32_t size,
                              ga10b_ctxsw_bootloader_desc_t* out);
/* find a region in a plain netlist image; returns 0 + off/size on hit */
int ga10b_fw_netlist_find(const void* buf, uint32_t size, uint32_t region_id,
                          uint32_t* off, uint32_t* rsize);
/* compute the per-falcon image desc from a boot desc + code/data sizes.
 * is_gpccs selects the aligned-resident-size variant (gr_falcon/acr). */
void ga10b_fw_ctxsw_image_desc(const ga10b_ctxsw_bootloader_desc_t* boot,
                               uint32_t code_size, uint32_t data_size,
                               int is_gpccs, ga10b_ls_image_desc_t* out);
/* total non-WPR blob size for n images (256-aligned) */
uint32_t ga10b_fw_ls_blob_size(const ga10b_ls_image_t* imgs, uint32_t n);
/* construct the LS blob into buf (cap bytes); wpr_base goes into the BL
 * DMA descriptors.  Returns bytes written or -1 on overflow/bad input. */
int ga10b_fw_build_ls_blob(void* buf, uint32_t cap,
                           const ga10b_ls_image_t* imgs, uint32_t n,
                           uint64_t wpr_base);

#endif