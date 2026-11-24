/*
 * GA10B ACR-GSP / WPR / LS-blob bring-up.
 *
 * Retail T234 has priv_sec_en burned: FECS/GPCCS can only boot LS through
 * the ACR high-secure ucode running on the GSP RISC-V core.  The driver
 * builds the non-WPR LS blob (wpr headers + PKC LSB headers + ucode images
 * + BL DMEM descriptors) in sysmem, hands its physical address to ACR via
 * the GSP mailboxes, and ACR authenticates/decrypts the images into the
 * WPR1 carveout (read-only, locked by MB1/MB2/BPMP) and bootstraps the LS
 * falcons.  No keys are ever touched here.
 *
 * Sources (OE4T linux-nvgpu rel-36, MIT):
 *   common/acr/acr_sw_ga10b.c        managed falcons, flcn2_acr_desc patch
 *   common/acr/acr_blob_construct.c  blob layout / LSB / BL desc fill
 *   common/acr/acr_bootstrap.c       HS boot + completion handshake
 *   common/riscv/riscv.c             nvgpu_riscv_hs_ucode_load_bootstrap
 *   common/gr/gr_falcon.c            ctxsw boot+code+data surface assembly
 *   hal/fb/fb_ga10b_fusa.c           ga10b_fb_read_wpr_info (WPR1 decode)
 *
 * ACR-managed LS-PMU (retail support_ls_pmu=true): the blob carries the PMU
 * next-core image (lazy_bootstrap=0, ACR bootstraps it) plus GPCCS+FECS with
 * lazy_bootstrap=1 (the PMU RTOS bootstraps them later, writing their HS
 * signatures).  ls_pmu_desc points at a sysmem copy of the PMU next-core
 * desc.  nvgpu runs FB/MM setup, FIFO init and nvgpu_gr_enable_hw (GR engine
 * + GPC reset) BEFORE the ACR executes -- see ga10bd.c stage_acr.  Pure blob
 * construction lives in ga10b_fw_parse.c (host-unit-testable); this file is
 * the DMA/register glue.
 */

#include <string.h>
#include <ewoksys/klog.h>

#include "ga10b.h"
#include "ga10b_regs.h"

static const ga10b_falcon_t gsp = { GA10B_GSP_FALCON_BASE, 1 };

static struct {
    int ready;
    ga10b_fw_set_t fw;
    uint64_t wpr_base;
    uint32_t wpr_size;
    void*    surf_va;  uint64_t surf_phy;  uint32_t surf_size;
    void*    blob_va;  uint64_t blob_phy;  uint32_t blob_size;
    void*    desc_va;  uint64_t desc_phy;
    void*    pmu_desc_va;  uint64_t pmu_desc_phy;
    ga10b_lsf_ucode_desc_wrapper_t fecs_sig, gpccs_sig, pmu_sig;
    ga10b_ls_image_t img[3];
} acr;

/* WPR1 is read-only on Tegra; decode val=(reg>>4)&0xfffffff, addr=val<<12
 * (fb_mmu_wpr1_addr_lo/hi_val_v + _alignment_v=12, fb_ga10b_fusa.c). */
int ga10b_wpr_read(uint64_t* base, uint32_t* size) {
    if (!ga10b.hw) {
        /* synthetic carveout so the dry-run blob build has addresses */
        *base = 0x80000000ULL;
        *size = 0x02000000ULL;
        klog("ga10b: [dry] WPR1 synthetic base=%llx size=%x\n",
             (unsigned long long)*base, *size);
        return 0;
    }
    uint32_t lo = ga10b_rd(GA10B_FB_WPR1_ADDR_LO);
    uint32_t hi = ga10b_rd(GA10B_FB_WPR1_ADDR_HI);
    uint64_t start = (uint64_t)((lo >> 4) & 0xfffffffu) << 12;
    uint64_t end   = (uint64_t)((hi >> 4) & 0xfffffffu) << 12;
    klog("ga10b: WPR1 lo=%08x hi=%08x -> base=%llx end=%llx\n",
         lo, hi, (unsigned long long)start, (unsigned long long)end);
    if (start == 0 || end <= start) {
        klog("ga10b: WPR1 not programmed\n");
        return -1;
    }
    *base = start;
    *size = (uint32_t)(end - start);
    return 0;
}

/* copy one ctxsw surface (boot@0, code@256, data@ALIGN) into dst */
static void copy_surface(uint8_t* dst, const ga10b_fw_t* enc,
                         const ga10b_ctxsw_bootloader_desc_t* boot,
                         const uint8_t* code, uint32_t code_size,
                         const uint8_t* data, uint32_t data_size,
                         const ga10b_ls_image_desc_t* d) {
    memcpy(dst + 0, (const uint8_t*)enc->va + 16, boot->size);
    memcpy(dst + d->app_start_offset, code, code_size);
    memcpy(dst + d->app_start_offset + d->app_resident_data_offset,
           data, data_size);
}

int ga10b_acr_init(const char* fw_dir) {
    if (acr.ready)
        return 0;

    if (ga10b_fw_set_load(fw_dir, &acr.fw) != 0)
        return -1;
    if (ga10b_wpr_read(&acr.wpr_base, &acr.wpr_size) != 0)
        goto fail;

    /* FECS/GPCCS boot descs (head of the 272B enc files) */
    ga10b_ctxsw_bootloader_desc_t fboot, gboot;
    if (ga10b_fw_parse_ctxsw_boot(acr.fw.fecs_enc.va, acr.fw.fecs_enc.size,
                                  &fboot) != 0 ||
        ga10b_fw_parse_ctxsw_boot(acr.fw.gpccs_enc.va, acr.fw.gpccs_enc.size,
                                  &gboot) != 0) {
        klog("ga10b: ctxsw boot desc parse failed\n");
        goto fail;
    }

    /* ctxsw code/data bodies from the NETC netlist (plain u32 regions) */
    const ga10b_fw_t* net = &acr.fw.net[2];
    uint32_t fi_off, fi_sz, fd_off, fd_sz, gi_off, gi_sz, gd_off, gd_sz;
    if (ga10b_fw_netlist_find(net->va, net->size, GA10B_NET_FECS_INST,
                              &fi_off, &fi_sz) != 0 ||
        ga10b_fw_netlist_find(net->va, net->size, GA10B_NET_FECS_DATA,
                              &fd_off, &fd_sz) != 0 ||
        ga10b_fw_netlist_find(net->va, net->size, GA10B_NET_GPCCS_INST,
                              &gi_off, &gi_sz) != 0 ||
        ga10b_fw_netlist_find(net->va, net->size, GA10B_NET_GPCCS_DATA,
                              &gd_off, &gd_sz) != 0) {
        klog("ga10b: NETC ctxsw regions missing\n");
        goto fail;
    }

    ga10b_ls_image_desc_t fd, gd;
    ga10b_fw_ctxsw_image_desc(&fboot, fi_sz, fd_sz, 0, &fd);
    ga10b_fw_ctxsw_image_desc(&gboot, gi_sz, gd_sz, 1, &gd);
    klog("ga10b: FECS surface %uB (boot imem@%x code=%u data=%u)\n",
         fd.image_size, fboot.imem_offset, fi_sz, fd_sz);
    klog("ga10b: GPCCS surface %uB (boot imem@%x code=%u data=%u)\n",
         gd.image_size, gboot.imem_offset, gi_sz, gd_sz);

    /* one DMA buffer holds both surfaces, FECS then GPCCS */
    acr.surf_size = fd.image_size + gd.image_size;
    if (ga10b_dma_alloc(acr.surf_size, 4096, &acr.surf_va, &acr.surf_phy) != 0)
        goto fail;
    uint8_t* sb = acr.surf_va;
    const uint8_t* nb = net->va;
    copy_surface(sb, &acr.fw.fecs_enc, &fboot, nb + fi_off, fi_sz,
                 nb + fd_off, fd_sz, &fd);
    copy_surface(sb + fd.image_size, &acr.fw.gpccs_enc, &gboot,
                 nb + gi_off, gi_sz, nb + gd_off, gd_sz, &gd);

    /* PKC sig wrappers copied verbatim, only falcon_id patched */
    memcpy(&acr.fecs_sig, acr.fw.fecs_sig.va, sizeof(acr.fecs_sig));
    memcpy(&acr.gpccs_sig, acr.fw.gpccs_sig.va, sizeof(acr.gpccs_sig));
    memcpy(&acr.pmu_sig, acr.fw.pmu_sig.va, sizeof(acr.pmu_sig));
    acr.fecs_sig.v2.falcon_id = GA10B_FALCON_FECS;
    acr.gpccs_sig.v2.falcon_id = GA10B_FALCON_GPCCS;
    acr.pmu_sig.v2.falcon_id = GA10B_FALCON_PMU_NEXT_CORE;

    /* ACR-managed LS-PMU (retail support_ls_pmu=true, acr_sw_ga10b.c):
     * PMU next-core(13) lazy=0/priv=0 -- ACR bootstraps it directly;
     * GPCCS(3) lazy=1/priv=1 and FECS(2) lazy=1/priv=0 -- bootstrapped
     * later by the PMU RTOS (which writes their HS signatures).
     * WPR header order follows nvgpu: images enumerated by ascending
     * falcon id and prepended (acr_blob_construct.c lsfm_add_ucode_img),
     * so PMU(13) leads GPCCS(3) leads FECS(2). */
    memset(acr.img, 0, sizeof(acr.img));
    acr.img[0].falcon_id = GA10B_FALCON_PMU_NEXT_CORE;
    acr.img[0].is_next_core = 1;
    acr.img[0].ndesc = &acr.fw.pmu;
    acr.img[0].data = acr.fw.pmu_image.va;
    acr.img[0].data_size = acr.fw.pmu_image.size;
    acr.img[0].sig = &acr.pmu_sig;
    acr.img[1].falcon_id = GA10B_FALCON_GPCCS;
    acr.img[1].lazy_bootstrap = 1;
    acr.img[1].is_priv_load = 1;
    acr.img[1].desc = gd;
    acr.img[1].data = sb + fd.image_size;
    acr.img[1].data_size = gd.image_size;
    acr.img[1].sig = &acr.gpccs_sig;
    acr.img[2].falcon_id = GA10B_FALCON_FECS;
    acr.img[2].lazy_bootstrap = 1;
    acr.img[2].is_priv_load = 0;
    acr.img[2].desc = fd;
    acr.img[2].data = sb;
    acr.img[2].data_size = fd.image_size;
    acr.img[2].sig = &acr.fecs_sig;

    acr.blob_size = ga10b_fw_ls_blob_size(acr.img, 3);
    if (acr.blob_size == 0 ||
        ga10b_dma_alloc(acr.blob_size, 4096, &acr.blob_va, &acr.blob_phy) != 0)
        goto fail;
    int built = ga10b_fw_build_ls_blob(acr.blob_va, acr.blob_size,
                                       acr.img, 3, acr.wpr_base);
    if (built < 0) {
        klog("ga10b: LS blob build failed\n");
        goto fail;
    }
    acr.blob_size = (uint32_t)built;

    /* ls_pmu_desc: sysmem copy of the PMU next-core desc handed to ACR HS
     * (ga10b_acr_patch_wpr_info_to_ucode copies sizeof(next_core_ucode_desc)
     * from fw_desc->data into a phys-addressed sysmem buffer). */
    if (ga10b_dma_alloc((uint32_t)sizeof(ga10b_pmu_ucode_desc_t), 256,
                        &acr.pmu_desc_va, &acr.pmu_desc_phy) != 0)
        goto fail;
    memcpy(acr.pmu_desc_va, acr.fw.pmu_desc.va,
           sizeof(ga10b_pmu_ucode_desc_t));

    /* flcn2_acr_desc in sysmem; ls_pmu_desc points at the PMU desc copy */
    if (ga10b_dma_alloc((uint32_t)sizeof(ga10b_flcn2_acr_desc_t), 256,
                        &acr.desc_va, &acr.desc_phy) != 0)
        goto fail;
    ga10b_fw_fill_acr_desc(acr.desc_va, acr.blob_phy, acr.blob_size,
                           acr.pmu_desc_phy);

    klog("ga10b: ACR ready: WPR1=%llx+%x blob=%u@%llx desc@%llx\n",
         (unsigned long long)acr.wpr_base, acr.wpr_size, acr.blob_size,
         (unsigned long long)acr.blob_phy, (unsigned long long)acr.desc_phy);
    acr.ready = 1;
    return 0;

fail:
    ga10b_fw_set_free(&acr.fw);
    if (acr.surf_va) { ga10b_dma_free(acr.surf_va); acr.surf_va = NULL; }
    if (acr.blob_va) { ga10b_dma_free(acr.blob_va); acr.blob_va = NULL; }
    if (acr.pmu_desc_va) { ga10b_dma_free(acr.pmu_desc_va); acr.pmu_desc_va = NULL; }
    if (acr.desc_va) { ga10b_dma_free(acr.desc_va); acr.desc_va = NULL; }
    return -1;
}

/* ACR is one-shot per GPU power cycle: once it seals WPR1 and bootstraps the
 * PMU next-core (lazy=0), re-running it fails with ACR_ERROR_REG_ACCESS_FAILURE
 * (0x1b) because the priv state is locked.  BPMP also denies CCPLEX pg_off
 * and reset on the GPU domain, so the state cannot be cleared from here.
 * Detect a previous successful run and let the caller skip the one-shot ACR.
 * With ACR-managed LS-PMU, ACR bootstraps the PMU (not FECS/GPCCS, which are
 * lazy=1 and bootstrapped later by the PMU RTOS).  Key on either the PMU RTOS
 * running (priv lockdown released => ACR bootstrapped PMU and it booted) or
 * the LS falcons FECS/GPCCS being bootstrapped (halted, clean mailbox0) after
 * the PMU RPC completed. */
int ga10b_acr_done(void) {
    if (!ga10b.hw || !ga10b.mapped)
        return 0;
    uint32_t pmu_hwcfg2 = ga10b_rd(GA10B_PMU_FALCON_BASE + GA10B_FALCON_HWCFG2);
    uint32_t pmu_brom = ga10b_rd(GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_BR_RETCODE);
    if ((pmu_brom & 3u) == 3u && (pmu_hwcfg2 & (1u << 13)) == 0)
        return 1;                  /* PMU RTOS running => ACR completed */
    uint32_t fecs_ctl = ga10b_rd(GA10B_FECS_FALCON_BASE + GA10B_FALCON_CPUCTL);
    uint32_t fecs_mb0 = ga10b_rd(GA10B_FECS_FALCON_BASE + GA10B_FALCON_MAILBOX0);
    uint32_t gpccs_ctl = ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_CPUCTL);
    uint32_t gpccs_mb0 = ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_MAILBOX0);
    return (fecs_ctl & (1u << 4)) && fecs_mb0 == 0 &&
           (gpccs_ctl & (1u << 4)) && gpccs_mb0 == 0;
}

/* ACR-GSP HS boot (nvgpu_riscv_hs_ucode_load_bootstrap + completion):
 * reset -> mailboxes=desc_pa -> BCR 0x11 -> PIO text->IMEM@0(sec),
 * data->DMEM@0, manifest->DMEM top -> startcpu -> BROM pass -> halt ->
 * MAILBOX0==0.  ACR runs to halt after bootstrapping the LS falcons. */
int ga10b_acr_boot_gsp(void) {
    if (!acr.ready)
        return -1;

    if (ga10b_falcon_reset(&gsp) != 0) {
        klog("ga10b: GSP reset failed\n");
        return -1;
    }
    ga10b_falcon_mbox_wr(&gsp, 0, (uint32_t)acr.desc_phy);
    ga10b_falcon_mbox_wr(&gsp, 1, (uint32_t)(acr.desc_phy >> 32));
    ga10b_priscv_set_bcr(&gsp);

    uint32_t hwcfg = ga10b_rd(GA10B_GSP_FALCON_BASE + GA10B_FALCON_HWCFG);
    uint32_t dmem_size = ((hwcfg >> 9) & 0x1ffu) << 8;
    uint32_t man_sz = acr.fw.acr_manifest.size;
    uint32_t man_off = dmem_size >= man_sz ? dmem_size - man_sz : 0;
    klog("ga10b: GSP hwcfg=%08x dmem=%u manifest %u@%x\n",
         hwcfg, dmem_size, man_sz, man_off);

    if (ga10b_falcon_pio_load(&gsp, 0, acr.fw.acr_text.va,
                              acr.fw.acr_text.size, 1, 0, NULL, 0) != 0)
        return -1;
    if (ga10b_falcon_pio_load(&gsp, 0, NULL, 0, 0,
                              0, acr.fw.acr_data.va,
                              acr.fw.acr_data.size) != 0)
        return -1;
    if (ga10b_falcon_pio_load(&gsp, 0, NULL, 0, 0,
                              man_off, acr.fw.acr_manifest.va, man_sz) != 0)
        return -1;

    if (ga10b_priscv_boot(&gsp, 0, 2000) != 0)
        return -1;
    if (ga10b_falcon_wait_halted(&gsp, 5000) != 0) {
        klog("ga10b: ACR-GSP halt timeout\n");
        return -1;
    }
    uint32_t mb0 = ga10b_falcon_mailbox(&gsp, 0);
    if (mb0 != 0) {
        klog("ga10b: ACR-GSP boot FAIL mailbox0=%08x mailbox1=%08x\n",
             mb0, ga10b_falcon_mailbox(&gsp, 1));
        return -1;
    }
    klog("ga10b: ACR-GSP HS boot OK (PMU bootstrapped into WPR1; "
         "FECS/GPCCS lazy, await PMU)\n");
    if (ga10b.hw && acr.blob_va) {
        const ga10b_lsf_wpr_header_t* wh = acr.blob_va;
        for (int i = 0; i < 3; i++)
            klog("ga10b: WPR hdr[%d] falcon=%u status=%u lazy=%u owner=%u "
                 "ver=%u\n", i, wh[i].falcon_id, wh[i].status,
                 wh[i].lazy_bootstrap, wh[i].bootstrap_owner, wh[i].bin_version);
    }
    return 0;
}

/* FECS/GPCCS are lazy_bootstrap=1: the PMU RTOS bootstraps them (writing their
 * HS signatures) via the FBQ RPC, done in stage_pmu before gr_init.  Nothing
 * for the ACR layer to do here (acr_sw_ga10b.c lazy path). */
int ga10b_acr_bootstrap_ls(uint32_t falcon_id) {
    if (ga10b.hw && !ga10b_pmu_gr_done()) {
        klog("ga10b: falcon %u has no HS signature (PMU bootstrap missing)\n",
             falcon_id);
        return -1;
    }
    klog("ga10b: falcon %u bootstrapped by PMU RTOS (lazy_bootstrap=1)\n",
         falcon_id);
    return 0;
}