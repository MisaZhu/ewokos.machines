/*
 * GA10B firmware blob parsing/validation -- pure logic, no OS deps.
 * Host-tested against the real prod blobs (test/test_fw.c).
 *
 * Layout sources cited in ga10b_fw.h.  The driver never decrypts or
 * verifies signatures itself: ACR HS does that on-chip with fuse-borne
 * keys.  These checks only catch missing/truncated/wrong-set files
 * before any hardware exposure.
 */

#include <string.h>

#include "ga10b_fw.h"

#define ALGN(x, a)  (((x) + ((a) - 1u)) & ~((a) - 1u))

/* on-wire layout must match nvgpu / the falcon BL exactly */
_Static_assert(sizeof(ga10b_lsf_ucode_desc_v2_t) == 2240u, "v2 desc size");
_Static_assert(sizeof(ga10b_lsf_ucode_desc_wrapper_t) == 2248u, "wrapper size");
_Static_assert(sizeof(ga10b_lsf_lsb_header_v1_t) == 2296u, "lsb v1 size");
_Static_assert(sizeof(ga10b_flcn_bl_dmem_desc_t) == 84u, "bl dmem desc size");
_Static_assert(sizeof(ga10b_flcn2_acr_desc_t) == 96u, "flcn2 acr desc size");
_Static_assert(sizeof(ga10b_lsf_wpr_header_t) == 24u, "wpr header size");

int ga10b_fw_parse_pmu_desc(const void* buf, uint32_t size,
                            uint32_t image_size, ga10b_pmu_ucode_desc_t* out) {
    if (buf == NULL || size < 60u)
        return -1;
    const uint32_t* w = buf;
    ga10b_pmu_ucode_desc_t d;
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < 15; i++)
        ((uint32_t*)&d)[i] = w[i];

    if (d.version == 0 || d.version > 0xffu)
        return -1;
    if (d.bootloader_size == 0 || d.next_core_elf_size == 0)
        return -1;
    /* every region must lie inside the image file */
    if (d.bootloader_offset + d.bootloader_size > image_size ||
        d.bootloader_param_offset + d.bootloader_param_size > image_size ||
        d.next_core_elf_offset + d.next_core_elf_size > image_size ||
        d.manifest_offset + d.manifest_size > image_size)
        return -1;
    if (d.is_monitor_enabled) {
        if (d.monitor_data_offset + d.monitor_data_size > image_size ||
            d.monitor_code_offset + d.monitor_code_size > image_size)
            return -1;
    }
    *out = d;
    return 0;
}

int ga10b_fw_check_pkc_sig(const void* buf, uint32_t size,
                           uint32_t falcon_id) {
    /* prod PKC sig file = 200B header + 2048B public key.  Observed
     * header invariants (36.4.3 blobs): w[0]=0x00020001, w[1]=total
     * file size (0x8c8), w[2]=falcon id.  Payload stays opaque. */
    if (buf == NULL || size != GA10B_PKC_SIG_FILE_SIZE)
        return -1;
    const uint32_t* w = buf;
    if (w[0] != 0x00020001u || w[1] != size || w[2] != falcon_id)
        return -1;
    return 0;
}

int ga10b_fw_check_ctxsw_enc(const void* buf, uint32_t size) {
    /* fecs/gpccs_encrypt_prod.bin: 16B boot desc + 256B boot image.
     * The boot image is encrypted (opaque); only the exact size and a
     * sane desc can be checked host-side. */
    if (buf == NULL || size != GA10B_CTXSW_ENC_HDR_SIZE)
        return -1;
    const uint8_t* b = buf;
    int nonzero = 0;
    for (uint32_t i = 0; i < size; i++)
        nonzero |= b[i];
    return nonzero ? 0 : -1;
}

int ga10b_fw_check_acr_set(uint32_t manifest_sz, uint32_t text_sz,
                           uint32_t data_sz) {
    /* all three are fully encrypted (no plaintext magic); only size
     * sanity is possible host-side.  Prod set: 2048 / 28672 / 9472. */
    if (manifest_sz == 0 || text_sz == 0 || data_sz == 0)
        return -1;
    if (manifest_sz % 256u != 0 || text_sz % 256u != 0 ||
        data_sz % 256u != 0)
        return -1;
    if (text_sz > 1024u * 1024u || data_sz > 1024u * 1024u ||
        manifest_sz > 64u * 1024u)
        return -1;
    return 0;
}

void ga10b_fw_fill_acr_desc(ga10b_flcn2_acr_desc_t* d,
                            uint64_t blob_phy, uint32_t blob_size,
                            uint64_t pmu_desc_phy) {
    memset(d, 0, sizeof(*d));
    /* wpr_region_id 0 + no_regions 1 + zeroed region props: ACR HS reads
     * the WPR1 bounds from hardware (acr_sw_ga10b.c / nvgpu_acr_interface.h). */
    d->wpr_region_id = 0;
    d->wpr_offset = 0;
    d->regions.no_regions = 1;
    d->nonwpr_ucode_blob_size = (blob_size + 255u) & ~255u;
    d->nonwpr_ucode_blob_start = blob_phy;
    d->ls_pmu_desc = pmu_desc_phy;
    d->gpu_mode = 0;   /* no emulate / MIG / simulation */
}

int ga10b_fw_parse_ctxsw_boot(const void* buf, uint32_t size,
                              ga10b_ctxsw_bootloader_desc_t* out) {
    if (buf == NULL || size != GA10B_CTXSW_ENC_HDR_SIZE)
        return -1;
    const uint32_t* w = buf;
    ga10b_ctxsw_bootloader_desc_t d;
    d.start_offset = w[0];
    d.size = w[1];
    d.imem_offset = w[2];
    d.entry_point = w[3];
    /* boot image is 256B; entry must match the load offset */
    if (d.size != 256u || d.entry_point != d.imem_offset)
        return -1;
    *out = d;
    return 0;
}

int ga10b_fw_netlist_find(const void* buf, uint32_t size, uint32_t region_id,
                          uint32_t* off, uint32_t* rsize) {
    if (buf == NULL || size < 8u)
        return -1;
    const uint32_t* w = buf;
    uint32_t version = w[0];
    uint32_t regions = w[1];
    if (version != 0 || regions == 0 || regions > 4096u)
        return -1;
    if (size < 8u + regions * 12u)
        return -1;
    for (uint32_t i = 0; i < regions; i++) {
        uint32_t id = w[2 + i * 3 + 0];
        uint32_t sz = w[2 + i * 3 + 1];
        uint32_t of = w[2 + i * 3 + 2];
        if (id != region_id)
            continue;
        if (sz == 0 || of < 8u + regions * 12u || of + sz > size)
            return -1;
        *off = of;
        *rsize = sz;
        return 0;
    }
    return -1;
}

void ga10b_fw_ctxsw_image_desc(const ga10b_ctxsw_bootloader_desc_t* boot,
                               uint32_t code_size, uint32_t data_size,
                               int is_gpccs, ga10b_ls_image_desc_t* out) {
    /* surface layout (gr_falcon.c): boot@0, code@ALIGN(boot,256),
     * data@ALIGN(code_end,256); boot is already 256B. */
    uint32_t boot_al = ALGN(boot->size, 256u);
    uint32_t code_off = boot_al;
    uint32_t code_al = ALGN(code_size, 256u);
    uint32_t data_off = ALGN(code_off + code_size, 256u);
    uint32_t data_al = ALGN(data_size, 256u);

    memset(out, 0, sizeof(*out));
    out->bootloader_start_offset = 0;
    out->bootloader_size = boot_al;
    out->bootloader_imem_offset = boot->imem_offset;
    out->bootloader_entry_point = boot->entry_point;
    out->image_size = boot_al + code_al + data_al;
    out->app_size = code_al + data_al;
    out->app_start_offset = code_off;
    out->app_resident_code_offset = 0;
    out->app_resident_data_offset = data_off - code_off;
    if (is_gpccs) {
        /* GPCCS uses 256-aligned resident sizes (acr_blob_construct.c) */
        out->app_resident_code_size = code_al;
        out->app_resident_data_size = data_al;
    } else {
        /* FECS keeps the raw netlist sizes */
        out->app_resident_code_size = code_size;
        out->app_resident_data_size = data_size;
    }
}

/* per-image layout offsets (lsf_calc_wpr_size_pkc + gen_wpr_requirements) */
typedef struct {
    uint32_t lsb_off;
    uint32_t ucode_off;
    uint32_t bl_data_off;
    uint32_t bl_data_size;
} ls_layout_t;

static uint32_t ls_layout(const ga10b_ls_image_t* imgs, uint32_t n,
                          ls_layout_t* lay) {
    uint32_t off = 24u * (n + 1u);   /* packed wpr headers + terminator */
    for (uint32_t i = 0; i < n; i++) {
        lay[i].lsb_off = ALGN(off, 256u);
        off = lay[i].lsb_off + (uint32_t)sizeof(ga10b_lsf_lsb_header_v1_t);
        lay[i].ucode_off = ALGN(off, 4096u);
        off = lay[i].ucode_off + imgs[i].data_size;
        lay[i].bl_data_size =
            ALGN((uint32_t)sizeof(ga10b_flcn_bl_dmem_desc_t), 256u);
        lay[i].bl_data_off = ALGN(off, 256u);
        off = lay[i].bl_data_off + lay[i].bl_data_size;
        if (imgs[i].is_next_core)
            off += GA10B_PMU_WPR_RSVD;
    }
    return ALGN(off, 256u);
}

uint32_t ga10b_fw_ls_blob_size(const ga10b_ls_image_t* imgs, uint32_t n) {
    ls_layout_t lay[4];
    if (n == 0 || n > 4)
        return 0;
    return ls_layout(imgs, n, lay);
}

static void fill_lsb(const ga10b_ls_image_t* img, const ls_layout_t* lay,
                     ga10b_lsf_lsb_header_v1_t* lsb) {
    memset(lsb, 0, sizeof(*lsb));
    if (img->sig != NULL)
        memcpy(&lsb->signature, img->sig, sizeof(lsb->signature));
    lsb->ucode_off = lay->ucode_off;
    lsb->bl_data_off = lay->bl_data_off;
    lsb->bl_data_size = lay->bl_data_size;

    if (img->is_next_core) {
        const ga10b_pmu_ucode_desc_t* nd = img->ndesc;
        lsb->ucode_size = nd->bootloader_offset + nd->bootloader_size +
                          nd->bootloader_param_size;
        uint32_t base = lsb->ucode_size + nd->next_core_elf_size;
        uint32_t pad = ALGN(base, 4096u) - base;
        lsb->data_size = nd->next_core_elf_size + pad;
        lsb->bl_code_size = 0;
        lsb->bl_imem_off = 0;
        /* bl_data_off/size keep the layout values: nvgpu zeroes them in the
         * static fill but lsf_calc_wpr_size_pkc sets them again for every
         * image, next-core included (no BL descriptor is written there). */
        return;
    }

    const ga10b_ls_image_desc_t* d = &img->desc;
    uint32_t bl_code = ALGN(d->bootloader_size, 256u);
    uint32_t full_app = ALGN(d->app_size, 256u) + bl_code;
    lsb->bl_code_size = bl_code;
    lsb->ucode_size = ALGN(d->app_resident_data_offset, 256u) + bl_code;
    lsb->data_size = full_app - lsb->ucode_size;
    lsb->bl_imem_off = d->bootloader_imem_offset;
    lsb->flags = img->is_priv_load ? GA10B_LSF_FLAG_FORCE_PRIV_LOAD : 0;
    lsb->app_code_off = bl_code;
    lsb->app_code_size = lsb->ucode_size - bl_code;
    lsb->app_data_off = lsb->ucode_size;
    lsb->app_data_size = lsb->data_size;
}

static void fill_bl_desc(const ga10b_ls_image_t* img,
                         const ga10b_lsf_lsb_header_v1_t* lsb,
                         uint64_t wpr_base, ga10b_flcn_bl_dmem_desc_t* bl) {
    memset(bl, 0, sizeof(*bl));
    const ga10b_ls_image_desc_t* d = &img->desc;
    uint64_t addr_base = wpr_base + lsb->ucode_off;
    uint64_t addr_code = addr_base + d->app_start_offset;
    uint64_t addr_data = addr_code + d->app_resident_data_offset;
    bl->ctx_dma = 0;   /* GK20A_PMU_DMAIDX_UCODE */
    bl->code_dma_base.lo = (uint32_t)addr_code;
    bl->code_dma_base.hi = (uint32_t)(addr_code >> 32);
    bl->non_sec_code_off = d->app_resident_code_offset;
    bl->non_sec_code_size = d->app_resident_code_size;
    bl->data_dma_base.lo = (uint32_t)addr_data;
    bl->data_dma_base.hi = (uint32_t)(addr_data >> 32);
    bl->data_size = d->app_resident_data_size;
    bl->code_entry_point = d->app_imem_entry;
    bl->argc = 1;      /* UCODE_PARAMS (LS_PMU build); argv unused FECS/GPCCS */
    bl->argv = 0;
}

int ga10b_fw_build_ls_blob(void* buf, uint32_t cap,
                           const ga10b_ls_image_t* imgs, uint32_t n,
                           uint64_t wpr_base) {
    if (buf == NULL || n == 0 || n > 4)
        return -1;
    ls_layout_t lay[4];
    uint32_t total = ls_layout(imgs, n, lay);
    if (cap < total)
        return -1;
    memset(buf, 0, total);
    uint8_t* b = buf;

    for (uint32_t i = 0; i < n; i++) {
        const ga10b_ls_image_t* img = &imgs[i];
        if (img->data == NULL || img->data_size == 0)
            return -1;

        ga10b_lsf_wpr_header_t wh;
        memset(&wh, 0, sizeof(wh));
        wh.falcon_id = img->falcon_id;
        wh.lsb_offset = lay[i].lsb_off;
        wh.bootstrap_owner = GA10B_FALCON_GSPLITE;
        wh.lazy_bootstrap = img->lazy_bootstrap;
        wh.bin_version = img->sig ? img->sig->v2.ls_ucode_version : 0;
        wh.status = GA10B_LSF_IMAGE_STATUS_COPY;
        memcpy(b + i * 24u, &wh, sizeof(wh));

        ga10b_lsf_lsb_header_v1_t lsb;
        fill_lsb(img, &lay[i], &lsb);
        memcpy(b + lay[i].lsb_off, &lsb, sizeof(lsb));

        if (!img->is_next_core) {
            ga10b_flcn_bl_dmem_desc_t bl;
            fill_bl_desc(img, &lsb, wpr_base, &bl);
            memcpy(b + lay[i].bl_data_off, &bl, sizeof(bl));
        }
        memcpy(b + lay[i].ucode_off, img->data, img->data_size);
    }

    ga10b_lsf_wpr_header_t term;
    memset(&term, 0, sizeof(term));
    term.falcon_id = GA10B_FALCON_INVALID;
    memcpy(b + n * 24u, &term, sizeof(term));
    return (int)total;
}