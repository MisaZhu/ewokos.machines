#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>

#include "ga10b.h"
#include "ga10b_regs.h"

/*
 * Falcon / RISC-V (falcon2+priscv) engine control.
 * Transcribed from nvgpu rel-36:
 *   hal/falcon/falcon_gk20a_fusa.c  (DMA/PIO load, bootstrap)
 *   hal/falcon/falcon_ga10b_fusa.c  (BCR/BROM, retcode, scrub)
 * priscv aperture = falcon base + 0x1000 (PMU 0x10a000/0x10b000,
 * GSP 0x110000/0x111000).
 */

static uint32_t frd(const ga10b_falcon_t* f, uint32_t off) {
    return ga10b_rd(f->base + off);
}
static void fwr(const ga10b_falcon_t* f, uint32_t off, uint32_t val) {
    ga10b_wr(f->base + off, val);
}
static uint32_t prd(const ga10b_falcon_t* f, uint32_t off) {
    return ga10b_rd(f->base + 0x1000 + off);
}
static void pwr(const ga10b_falcon_t* f, uint32_t off, uint32_t val) {
    ga10b_wr(f->base + 0x1000 + off, val);
}

/* nvgpu_falcon_reset: engine-dependent reset, then IMEM/DMEM scrub wait
 * (1 ms max, 10 us steps).  GSP uses its engine reset register
 * (ga10b_gsp_engine_reset); other falcons fall back to a CPU hard reset
 * (gk20a_falcon_reset: cpuctl |= hreset). */
int ga10b_falcon_reset(const ga10b_falcon_t* f) {
    if (f->base == GA10B_GSP_FALCON_BASE) {
        ga10b_wr(GA10B_GSP_FALCON_ENGINE, 1);
        ga10b_delay_us(10);
        ga10b_wr(GA10B_GSP_FALCON_ENGINE, 0);
    } else {
        fwr(f, GA10B_FALCON_CPUCTL,
            frd(f, GA10B_FALCON_CPUCTL) | (1u << 3));
    }
    if (!ga10b.hw)
        return 0;
    for (uint32_t i = 0; i < 100u; i++) {
        if (!(frd(f, GA10B_FALCON_HWCFG2) & (1u << 12)))
            return 0;
        ga10b_delay_us(10);
    }
    klog("ga10b: falcon@%08x mem scrub timeout (hwcfg2=%08x)\n", f->base,
         frd(f, GA10B_FALCON_HWCFG2));
    return -1;
}

uint32_t ga10b_falcon_mailbox(const ga10b_falcon_t* f, int idx) {
    return frd(f, GA10B_FALCON_MAILBOX0 + 4u * (uint32_t)idx);
}

void ga10b_falcon_mbox_wr(const ga10b_falcon_t* f, int idx, uint32_t val) {
    fwr(f, GA10B_FALCON_MAILBOX0 + 4u * (uint32_t)idx, val);
}

uint32_t ga10b_falcon_hwcfg2(const ga10b_falcon_t* f) {
    return frd(f, GA10B_FALCON_HWCFG2);
}

int ga10b_falcon_wait_idle(const ga10b_falcon_t* f, uint32_t tries) {
    return ga10b_poll(f->base + GA10B_FALCON_IDLESTATE, 1, 0, tries);
}

int ga10b_falcon_wait_halted(const ga10b_falcon_t* f, uint32_t tries) {
    uint32_t reg = f->riscv ?
        f->base + 0x1000 + GA10B_PRISCV_CPUCTL :
        f->base + GA10B_FALCON_CPUCTL;
    return ga10b_poll(reg, 1u << 4, 1u << 4, tries);
}

/* DMA load: 256B chunks, DMEM first then IMEM (gk20a_falcon_load_ucode_dma).
 * phy = DMA buffer base; imem/dmem VAs must live in the same dma pool so
 * source offsets can be recovered via dma_phy_addr. */
static int dma_seg(const ga10b_falcon_t* f, uint64_t phy, const void* va,
                   uint32_t dst_off, uint32_t size, int imem) {
    uint64_t src_pa = dma_phy_addr(GA10B_DMA_BLOCK, (void*)va);
    uint32_t src_off = (uint32_t)(src_pa - phy);
    for (uint32_t i = 0; i < size; i += 256) {
        fwr(f, GA10B_FALCON_DMATRFMOFFS, dst_off + i);
        fwr(f, GA10B_FALCON_DMATRFFBOFFS, src_off + i);
        fwr(f, GA10B_FALCON_DMATRFCMD,
            (imem ? (1u << 4) : 0) | (6u << 8));   /* write=0, size=256B */
        if (ga10b_poll(f->base + GA10B_FALCON_DMATRFCMD, 1u << 1, 1u << 1,
                       1000) != 0)
            return -1;
    }
    return 0;
}

int ga10b_falcon_dma_load(const ga10b_falcon_t* f, uint64_t phy,
                          uint32_t imem_off, const void* imem, uint32_t imem_sz,
                          uint32_t dmem_off, const void* dmem, uint32_t dmem_sz) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] falcon@%08x dma load imem %u@%x dmem %u@%x\n",
             f->base, imem_sz, imem_off, dmem_sz, dmem_off);
        return 0;
    }
    fwr(f, GA10B_FALCON_DMACTL, 0);
    fwr(f, GA10B_FALCON_DMATRFBASE, (uint32_t)(phy >> 8));
    fwr(f, GA10B_FALCON_DMATRFBASE1, (uint32_t)(phy >> 40));
    if (dmem && dmem_sz &&
        dma_seg(f, phy, dmem, dmem_off, dmem_sz, 0) != 0)
        return -1;
    if (imem && imem_sz &&
        dma_seg(f, phy, imem, imem_off, imem_sz, 1) != 0)
        return -1;
    return 0;
}

/* PIO load (gk20a_falcon_copy_to_imem/dmem).  falcon2 (riscv) has no imem
 * tags; classic falcons get a tag per 256B block. */
int ga10b_falcon_pio_load(const ga10b_falcon_t* f,
                          uint32_t imem_off, const void* imem, uint32_t imem_sz,
                          int sec,
                          uint32_t dmem_off, const void* dmem, uint32_t dmem_sz) {
    if (!ga10b.hw) {
        klog("ga10b: [dry] falcon@%08x pio load imem %u@%x sec=%d dmem %u@%x\n",
             f->base, imem_sz, imem_off, sec, dmem_sz, dmem_off);
        return 0;
    }
    if (dmem && dmem_sz) {
        const uint32_t* w = (const uint32_t*)dmem;
        uint32_t words = (dmem_sz + 3) / 4;
        fwr(f, GA10B_FALCON_DMEMC(0), dmem_off | GA10B_FALCON_DMEMC_AINCW);
        for (uint32_t i = 0; i < words; i++)
            fwr(f, GA10B_FALCON_DMEMD(0), w[i]);
    }
    if (imem && imem_sz) {
        const uint32_t* w = (const uint32_t*)imem;
        uint32_t words = (imem_sz + 3) / 4;
        fwr(f, GA10B_FALCON_IMEMC(0),
            GA10B_FALCON_IMEMC_OFFS(imem_off >> 2) |
            GA10B_FALCON_IMEMC_BLK(imem_off >> 8) |
            GA10B_FALCON_IMEMC_AINCW |
            (sec ? GA10B_FALCON_IMEMC_SECURE : 0));
        uint32_t tag = imem_off >> 8;
        for (uint32_t i = 0; i < words; i++) {
            if (!f->riscv && (i % 64) == 0) {
                fwr(f, GA10B_FALCON_IMEMT(0), tag);
                tag++;
            }
            fwr(f, GA10B_FALCON_IMEMD(0), w[i]);
        }
        while ((words % 64) != 0) {          /* zero-pad to block boundary */
            fwr(f, GA10B_FALCON_IMEMD(0), 0);
            words++;
        }
    }
    return 0;
}

int ga10b_falcon_start(const ga10b_falcon_t* f, uint32_t bootvec) {
    if (f->riscv)
        return ga10b_priscv_boot(f, bootvec, 0);
    fwr(f, GA10B_FALCON_DMACTL, 0);          /* require_ctx = 0 */
    fwr(f, GA10B_FALCON_BOOTVEC, bootvec);
    fwr(f, GA10B_FALCON_CPUCTL, 1u << 1);    /* startcpu */
    return 0;
}

void ga10b_priscv_set_bcr(const ga10b_falcon_t* f) {
    pwr(f, GA10B_PRISCV_BCR_CTRL, 0x11);
}

/* ga10b_priscv_brom_config: writes lo32/hi32 of the passed addresses into
 * the BCR addr regs.  The PMU caller passes (image_pa + offset) >> 8
 * (256B units, same convention as falcon dmatrfbase), NOT raw bytes. */
int ga10b_priscv_brom_config(const ga10b_falcon_t* f, uint64_t code_pa,
                             uint64_t data_pa, uint64_t manifest_pa) {
    pwr(f, GA10B_PRISCV_BCR_FMCCODE_LO, (uint32_t)code_pa);
    pwr(f, GA10B_PRISCV_BCR_FMCCODE_HI, (uint32_t)(code_pa >> 32));
    pwr(f, GA10B_PRISCV_BCR_FMCDATA_LO, (uint32_t)data_pa);
    pwr(f, GA10B_PRISCV_BCR_FMCDATA_HI, (uint32_t)(data_pa >> 32));
    pwr(f, GA10B_PRISCV_BCR_PKCPARAM_LO, (uint32_t)manifest_pa);
    pwr(f, GA10B_PRISCV_BCR_PKCPARAM_HI, (uint32_t)(manifest_pa >> 32));
    pwr(f, GA10B_PRISCV_BCR_DMACFG, 0x80000002u);  /* ncoh sys + locked */
    pwr(f, GA10B_PRISCV_BCR_CTRL, 0x111);
    return 0;
}

/* Start the RISC-V core and (tries>0) wait for BROM pass retcode==3. */
int ga10b_priscv_boot(const ga10b_falcon_t* f, uint64_t bootvec, uint32_t tries) {
    if (bootvec) {
        pwr(f, GA10B_PRISCV_BOOT_VECTOR_LO, (uint32_t)bootvec);
        pwr(f, GA10B_PRISCV_BOOT_VECTOR_HI, (uint32_t)(bootvec >> 32));
    }
    pwr(f, GA10B_PRISCV_CPUCTL, 1);
    if (!ga10b.hw || tries == 0)
        return 0;
    for (uint32_t i = 0; i < tries; i++) {
        uint32_t rc = prd(f, GA10B_PRISCV_BR_RETCODE) & 3;
        if (rc == 3)
            return 0;
        if (rc == 2) {
            klog("ga10b: priscv@%08x BROM FAIL retcode=%08x\n",
                 f->base, prd(f, GA10B_PRISCV_BR_RETCODE));
            return -1;
        }
        ga10b_delay_us(1000);
    }
    klog("ga10b: priscv@%08x BROM timeout retcode=%08x\n",
         f->base, prd(f, GA10B_PRISCV_BR_RETCODE));
    return -1;
}