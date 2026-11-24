/*
 * Copy Engine (AMPERE_DMA_COPY_B, 0xC7B5) for GA10B.
 *
 * Engine init (nvgpu common/ce/ce.c nvgpu_ce_init_support): MC reset of
 * every LCE device -> force_barriers_npl prod -> stall/nonstall interrupt
 * routing to CPU (ce_ga10b_fusa.c).  Per-LCE registers are indexed by the
 * TOP device instance id.
 *
 * Blit methods follow clc7b5.h and are emitted on subchannel 4 like
 * nvgpu common/ce/ce_app.c.  Unlike ce_app.c (physical-mode transfers
 * without a semaphore), this stream uses GMMU virtual src/dst addresses
 * (ga10b_gmmu_map output) plus a CE semaphore release; that variant is
 * taken from the class header only and is unproven on hardware.
 */

#include <ewoksys/klog.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"

#define CE_INTR_EN_MASK    0x8000007Fu   /* ce_lce_intr_mask: blockpipe/
                                          * nonblockpipe/launcherr/
                                          * invalid_config/mthd_buffer_fault/
                                          * fbuf_crc/magic + stalling debug */
#define CE_INTR_CPU_GSP_M  0xC0000000u   /* cpu bit31 | gsp bit30 */
#define CE_BLIT_WORDS      20u
#define CE_SEM_TRIES       200000u       /* ~2s at 10us */

static void ce_lce_setup(uint32_t i) {
    ga10b_wr(GA10B_CE_LCE_OPT(i),
             ga10b_rd(GA10B_CE_LCE_OPT(i)) | GA10B_CE_LCE_OPT_FORCE_BARR_NPL);
    /* keep the POR vector ids; stalling -> CPU, GSP reporting off.  The
     * nonstalling (notify) tree is off for both, as in the nvgpu Linux
     * build (CONFIG_NVGPU_NONSTALL_INTR branch of ce_ga10b_fusa.c). */
    uint32_t nctrl = ga10b_rd(GA10B_CE_LCE_INTR_NOTIFY_CTRL(i));
    uint32_t ctrl = ga10b_rd(GA10B_CE_LCE_INTR_CTRL(i));
    ga10b_wr(GA10B_CE_LCE_INTR_CTRL(i),
             (ctrl & ~CE_INTR_CPU_GSP_M) | GA10B_CE_INTR_CPU_ENABLE);
    ga10b_wr(GA10B_CE_LCE_INTR_NOTIFY_CTRL(i), nctrl & ~CE_INTR_CPU_GSP_M);
    ga10b_wr(GA10B_CE_LCE_INTR_EN(i), CE_INTR_EN_MASK);
}

int ga10b_ce_init(void) {
    /* nvgpu_mc_reset_devtype(LCE): disable all LCEs, then enable all
     * (Bug 1895019).  Dry-run has no device table: program both LCEs. */
    ga10b_devtable_t t;
    uint32_t insts = 0, n = 0;
    if (ga10b_device_table(&t) != 0)
        return -1;
    for (int en = 0; en <= 1; en++)
        for (uint32_t i = 0; i < t.count; i++)
            if (t.dev[i].type == GA10B_DEVTYPE_LCE &&
                ga10b_engine_enable(t.dev[i].reset_id, en) != 0)
                return -1;
    for (uint32_t i = 0; i < t.count; i++)
        if (t.dev[i].type == GA10B_DEVTYPE_LCE &&
            t.dev[i].inst_id < GA10B_CE_NUM_LCE_ALL)
            insts |= 1u << t.dev[i].inst_id;
    if (insts == 0) {
        if (ga10b.hw) {
            klog("ga10b: no LCE in the TOP device table\n");
            return -1;
        }
        insts = (1u << GA10B_CE_NUM_LCE_ALL) - 1u;
    }
    for (uint32_t i = 0; i < GA10B_CE_NUM_LCE_ALL; i++) {
        if (!(insts & (1u << i)))
            continue;
        ce_lce_setup(i);
        n++;
    }
    klog("ga10b: ce init ok, %u LCE(s)\n", n);
    return 0;
}

int ga10b_ce_blit(ga10b_chan_t* ch,
                  uint64_t src_va, uint32_t src_pitch,
                  uint64_t dst_va, uint32_t dst_pitch,
                  uint32_t width_bytes, uint32_t height) {
    uint32_t* pb = ga10b_pb_reserve(ch, CE_BLIT_WORDS);
    if (pb == NULL)
        return -1;
    uint32_t start = ch->pb_put - CE_BLIT_WORDS;

    uint32_t sem_payload = ++ch->sem_val;
    uint32_t launch = GA10B_CE_LAUNCH_NON_PIPELINED |
                      GA10B_CE_LAUNCH_FLUSH_ENABLE |
                      GA10B_CE_LAUNCH_SEM_ONE_WORD |
                      GA10B_CE_LAUNCH_SRC_PITCH |
                      GA10B_CE_LAUNCH_DST_PITCH;
    if (height > 1)
        launch |= GA10B_CE_LAUNCH_MULTI_LINE;

    uint32_t n = 0;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_MTHD_SET_OBJECT, 1);
    pb[n++] = GA10B_CLASS_DMA_COPY_B;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_SET_SEMAPHORE_A, 3);
    pb[n++] = (uint32_t)(ch->sem_gpu_va >> 32) & 0x1ffffu;
    pb[n++] = (uint32_t)ch->sem_gpu_va;
    pb[n++] = sem_payload;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_OFFSET_IN_UPPER, 2);
    pb[n++] = (uint32_t)(src_va >> 32);
    pb[n++] = (uint32_t)src_va;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_OFFSET_OUT_UPPER, 2);
    pb[n++] = (uint32_t)(dst_va >> 32);
    pb[n++] = (uint32_t)dst_va;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_PITCH_IN, 2);
    pb[n++] = src_pitch;
    pb[n++] = dst_pitch;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_LINE_LENGTH_IN, 2);
    pb[n++] = width_bytes;
    pb[n++] = height;
    pb[n++] = ga10b_pb_inc(GA10B_SUBCH_CE, GA10B_CE_LAUNCH_DMA, 1);
    pb[n++] = launch;

    ga10b_chan_push(ch, start, n);
    klog("ga10b: ce blit %ux%u src=%llx dst=%llx sem=%u launch=%08x\n",
         width_bytes, height,
         (unsigned long long)src_va, (unsigned long long)dst_va,
         sem_payload, launch);
    ga10b_chan_submit(ch);
    return ga10b_chan_wait_sem(ch, sem_payload, CE_SEM_TRIES);
}