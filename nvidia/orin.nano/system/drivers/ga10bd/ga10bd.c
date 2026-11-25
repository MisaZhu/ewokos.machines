/*
 * ga10bd -- standalone GA10B (Orin Nano GPU) bring-up CLI.
 *
 * NOT wired into init.rd or the system build: nothing runs at boot.
 * Every stage is an explicit subcommand; hardware access additionally
 * requires --hw.  Without --hw the tool runs in dry-run mode: register
 * writes are logged, reads return 0, so sequences can be reviewed with
 * zero board exposure.
 *
 * State does not persist across invocations: `ce` and `gr` re-run the
 * whole prerequisite chain in-process.
 *
 * Firmware blobs (NVIDIA proprietary, user-supplied) are read from
 * --fw-dir (default /fs/nvidia/ga10b), extracted from the L4T
 * nvidia-l4t-firmware deb.  See docs/gpu-accel-research.md §3.5.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"

static const char* _fw_dir = GA10B_FW_DIR_DEFAULT;
static const char* _cubin = NULL;

static int cmd_probe(void) {
    klog("ga10bd: probe (hw=%d)\n", ga10b.hw);
    if (ga10b_power_on() != 0)
        return -1;
    if (ga10b_map() != 0)
        return -1;
    if (ga10b_detect() != 0)
        return -1;
    if (ga10b_fuse_read() != 0)
        return -1;
    if (ga10b_gpc_clk_init() != 0)
        return -1;
    klog("ga10bd: chip_id=0x%x priv_sec_en=%d gpcs=%u gpusys=%llu Hz\n",
         ga10b.chip_id, ga10b.priv_sec_en, ga10b.gpc_count,
         (unsigned long long)ga10b.gpusys_hz);
    if (ga10b.hw) {
        klog("ga10bd: gsp cpuctl=%08x mb0=%08x mb1=%08x | fecs cpuctl=%08x "
             "mb0=%08x | gpccs cpuctl=%08x mb0=%08x | acr_done=%d\n",
             ga10b_rd(GA10B_GSP_PRISCV_BASE + GA10B_PRISCV_CPUCTL),
             ga10b_rd(GA10B_GSP_FALCON_BASE + GA10B_FALCON_MAILBOX0),
             ga10b_rd(GA10B_GSP_FALCON_BASE + GA10B_FALCON_MAILBOX1),
             ga10b_rd(GA10B_FECS_CPUCTL),
             ga10b_rd(GA10B_FECS_FALCON_BASE + GA10B_FALCON_MAILBOX0),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_CPUCTL),
             ga10b_rd(GA10B_GPCCS_FALCON_BASE + GA10B_FALCON_MAILBOX0),
             ga10b_acr_done());
    }
    return 0;
}

static int cmd_ce(void) {
    /* M0 acceptance: full chain minus GR/PMU-RTOS, then a CPU-verified
     * pitch blit through a CE channel. */
    if (ga10b_stage_acr(_fw_dir) != 0)
        return -1;

    ga10b_gmmu_t mm;
    if (ga10b_gmmu_init(&mm) != 0)
        return -1;
    if (ga10b_fifo_init() != 0)
        return -1;
    if (ga10b_ce_init() != 0)
        return -1;

    ga10b_chan_t ch;
    if (ga10b_chan_create(&mm, &ch, GA10B_DEVTYPE_LCE, 2) != 0)
        return -1;

    enum { W = 256, H = 64, PITCH = W * 4 };
    void *src, *dst;
    uint64_t src_phy, dst_phy;
    if (ga10b_dma_alloc(PITCH * H, 4096, &src, &src_phy) != 0 ||
        ga10b_dma_alloc(PITCH * H, 4096, &dst, &dst_phy) != 0)
        return -1;
    uint32_t* s = src;
    for (uint32_t i = 0; i < W * H; i++)
        s[i] = 0xff000000u | (i * 2654435761u);
    memset(dst, 0, PITCH * H);

    uint64_t src_va = ga10b_gmmu_map(&mm, src_phy, PITCH * H);
    uint64_t dst_va = ga10b_gmmu_map(&mm, dst_phy, PITCH * H);
    if (src_va == 0 || dst_va == 0)
        return -1;

    int r = ga10b_ce_blit(&ch, src_va, PITCH, dst_va, PITCH, PITCH, H);
    if (r == 0) {
        if (memcmp(src, dst, PITCH * H) == 0) {
            klog("ga10bd: CE blit PASS (%ux%u)\n", W, H);
        } else {
            klog("ga10bd: CE blit data MISMATCH\n");
            r = -1;
        }
    } else {
        volatile uint32_t* ud = (volatile uint32_t*)ch.userd_va;
        uint32_t* d = dst;
        klog("ga10bd: userd gp_get=%u gp_put=%u pb_get=%u pb_put=%u\n",
             ud[GA10B_USERD_GP_GET], ud[GA10B_USERD_GP_PUT],
             ud[GA10B_USERD_PB_GET], ud[GA10B_USERD_PB_PUT]);
        klog("ga10bd: dst[0..3]=%08x %08x %08x %08x (want src[0]=%08x)\n",
             d[0], d[1], d[2], d[3], s[0]);
        /* runqueue: did PBDMA post any methods?  word0=count word1=chid */
        volatile uint32_t* rq = (volatile uint32_t*)ch.rq_va;
        klog("ga10bd: rq_va=%p rq_gpu_va=%llx rq[0..7]=%08x %08x %08x %08x "
             "%08x %08x %08x %08x\n", ch.rq_va,
             (unsigned long long)ch.rq_gpu_va,
             rq[0], rq[1], rq[2], rq[3], rq[4], rq[5], rq[6], rq[7]);
        /* instance block readback: RAMFC target/subdevice + RAMIN pdb/mthdbuf */
        volatile uint32_t* ib = (volatile uint32_t*)ch.inst_va;
        klog("ga10bd: inst sig=%08x subdev=%08x target=%08x config=%08x "
             "gpbase=%08x.%08x\n",
             ib[GA10B_RAMFC_SIGNATURE], ib[GA10B_RAMFC_SUBDEVICE],
             ib[GA10B_RAMFC_TARGET], ib[GA10B_RAMFC_CONFIG],
             ib[GA10B_RAMFC_GP_BASE_HI], ib[GA10B_RAMFC_GP_BASE]);
        klog("ga10bd: inst pdb=%08x.%08x mthdbuf=%08x.%08x magic=%08x\n",
             ib[GA10B_RAMIN_PDB_HI], ib[GA10B_RAMIN_PDB_LO],
             ib[GA10B_RAMIN_ENG_MTHD_BUF_HI], ib[GA10B_RAMIN_ENG_MTHD_BUF_LO],
             ib[GA10B_RAMIN_FW_MAGIC]);
        ga10b_gmmu_dump(&mm, ch.gpfifo_gpu_va);
        ga10b_gmmu_dump(&mm, ch.pb_gpu_va);
        ga10b_diag_dump();
    }
    ga10b_chan_destroy(&ch);
    ga10b_gmmu_destroy(&mm);
    ga10b_dma_free(src);
    ga10b_dma_free(dst);
    return r;
}

/* compute channel on the GRAPHICS runlist + one launch: the nvcc sm_87 vadd
 * kernel on hardware, the placeholder QMD trace in dry-run.  pgl < 0 is the
 * takeover test: after the launch the process dies the way a crashed GPU
 * owner does, with its channel still scheduled and no exit handler run. */
static int cmd_compute(int pgl) {
    if (ga10b_stage_gr(_fw_dir) != 0)
        return -1;
    ga10b_gmmu_t mm;
    if (ga10b_gmmu_init(&mm) != 0)
        return -1;
    if (ga10b_fifo_init() != 0)
        return -1;
    /* the GR runlist also carries the grcopy LCEs and RAMFC target marks the
     * CE ctx valid: with them held in reset PBDMA raises eng_reset and stalls
     * (nvgpu_ce_init_support always runs before the first channel) */
    if (ga10b_ce_init() != 0)
        return -1;
    ga10b_chan_t ch;
    if (ga10b_chan_create(&mm, &ch, GA10B_DEVTYPE_GRAPHICS, 0) != 0)
        return -1;
#ifdef GA10B_HAVE_CUDA
    int r;
    if (pgl > 0)
        r = ga10b_pgl_selftest(&ch, _cubin != NULL ? _cubin :
                               "/fs/cuda/pgl_geom.sm_87.cubin");
    else if (ga10b.hw)
        r = ga10b_compute_vadd(&ch, _cubin != NULL ? _cubin :
                               "/fs/cuda/vectorAdd.sm_87.cubin");
    else
        r = ga10b_compute_dispatch(&ch, 0);
#else
    (void)_cubin;
    (void)pgl;
    int r = ga10b_compute_dispatch(&ch, 0);
#endif
    if (r != 0 && ga10b.hw)
        ga10b_diag_dump();
    if (pgl < 0 && ga10b.hw) {
        klog("ga10bd: orphan -> %d, dying with chan%u scheduled\n", r, ch.chid);
        syscall1(SYS_EXIT, (ewokos_addr_t)(r == 0 ? 0 : 1));
    }
    ga10b_chan_destroy(&ch);
    ga10b_gmmu_destroy(&mm);
    return r;
}

static int cmd_diag(void) {
    if (ga10b_power_on() != 0)
        return -1;
    if (ga10b_map() != 0)
        return -1;
    ga10b_diag_dump();
    return 0;
}

static int cmd_dump(void) {
    if (ga10b_power_on() != 0)
        return -1;
    if (ga10b_map() != 0)
        return -1;
    ga10b_gr_falcon_memdump();
    return 0;
}

static void usage(void) {
    klog("usage: ga10bd [--hw] [--fw-dir PATH] [--cubin FILE] [--skip MASK] "
         "<cmd>\n"
         "  fw         load+validate all firmware blobs (no MMIO)\n"
         "  probe      power+map+detect+fuse (read-only)\n"
         "  init       power+detect+intr-mask+priv-ring+bus+cg\n"
         "  acr        init + FB + FIFO + GR enable + ACR-GSP HS boot (WPR1)\n"
         "  pmu        acr + PMU RTOS boot + RPC bootstrap FECS/GPCCS (LS-PMU)\n"
         "  ce         acr + GMMU + CE blit self-test (M0)\n"
         "  gr         acr + pmu + FECS/GPCCS start + golden ctx\n"
         "  compute    gr + compute channel + nvcc sm_87 vadd launch (M1)\n"
         "  raster     gr + portablegl geometry kernels vs CPU reference\n"
         "  orphan     compute, then die without teardown (takeover test)\n"
         "  diag       dump runlist/PBDMA/CE/GR/FECS state (read-only)\n"
         "  dump       hex-dump FECS/GPCCS IMEM+DMEM (after a failed gr)\n"
         "  power-off  BPMP powergate off\n"
         "Without --hw everything is dry-run (log only, no MMIO).\n"
         "--skip leaves out GR init steps: 1 gfx non-ctx lists, 2 live PES\n"
         "  mask, 4 PES sm-id order, 8 SLCG/BLCG prods, 10 SMC timestamp,\n"
         "  20 setup_hw before falcons, 40 compute-only eng_config,\n"
         "  80 no PMU ISR emulation (dmesg ring is then dumped read-only),\n"
         "  100 PMU ISR emulation without the dmesg read-offset ack,\n"
         "  200 MIG-style sw_ctx_load (graphics registers filtered out).\n");
}

int main(int argc, char** argv) {
    const char* cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hw") == 0) {
            ga10b.hw = 1;
        } else if (strcmp(argv[i], "--fw-dir") == 0 && i + 1 < argc) {
            _fw_dir = argv[++i];
        } else if (strcmp(argv[i], "--cubin") == 0 && i + 1 < argc) {
            _cubin = argv[++i];
        } else if (strcmp(argv[i], "--skip") == 0 && i + 1 < argc) {
            ga10b.skip = (uint32_t)strtoul(argv[++i], NULL, 16);
        } else if (argv[i][0] != '-') {
            cmd = argv[i];
        }
    }
    if (cmd == NULL) {
        usage();
        return 1;
    }
    klog("ga10bd: cmd=%s hw=%d fw=%s skip=%x\n", cmd, ga10b.hw, _fw_dir,
         ga10b.skip);

    int r;
    if (strcmp(cmd, "fw") == 0)
        r = ga10b_fw_inventory(_fw_dir);
    else if (strcmp(cmd, "probe") == 0)
        r = cmd_probe();
    else if (strcmp(cmd, "init") == 0)
        r = ga10b_stage_init();
    else if (strcmp(cmd, "acr") == 0)
        r = ga10b_stage_acr(_fw_dir);
    else if (strcmp(cmd, "pmu") == 0) {
        r = ga10b_stage_acr(_fw_dir);
        if (r == 0) r = ga10b_stage_pmu(_fw_dir);
    } else if (strcmp(cmd, "ce") == 0)
        r = cmd_ce();
    else if (strcmp(cmd, "gr") == 0)
        r = ga10b_stage_gr(_fw_dir);
    else if (strcmp(cmd, "compute") == 0)
        r = cmd_compute(0);
    else if (strcmp(cmd, "raster") == 0)
        r = cmd_compute(1);
    else if (strcmp(cmd, "orphan") == 0)
        r = cmd_compute(-1);
    else if (strcmp(cmd, "diag") == 0)
        r = cmd_diag();
    else if (strcmp(cmd, "dump") == 0)
        r = cmd_dump();
    else if (strcmp(cmd, "power-off") == 0)
        r = ga10b_power_off();
    else {
        usage();
        return 1;
    }
    klog("ga10bd: %s -> %d\n", cmd, r);
    return r == 0 ? 0 : 1;
}