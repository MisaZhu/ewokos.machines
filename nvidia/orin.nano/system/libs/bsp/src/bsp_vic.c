#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <bsp/bsp_vic.h>
#include <bsp/bsp_bpmp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>
#include <orin_net.h>

/*
 * Tegra234 VIC bring-up (milestone 1).  Userspace, polled, no IRQs.
 *
 * Register layout and sequences are taken from:
 *  - mainline drivers/gpu/host1x (host1x08: dev.c/channel_hw.c/cdma_hw.c/
 *    syncpt_hw.c, hw_host1x08_{vm,common,uclass,hypervisor}.h, opcodes.h)
 *  - mainline drivers/gpu/drm/tegra/{vic.c,falcon.c,falcon.h}
 *  - Orin TRM DP-10508-002 v1.2p §7.3 (VIC THI/falcon registers, method
 *    interface, syncpt conditions 0=IMMEDIATE 1=OP_DONE 4=ENGINE_IDLE)
 *
 * host1x apertures (tegra234.dtsi host1x@13e00000):
 *   common 0x13e00000 (MLOCKs), hypervisor 0x13e10000 (VM/SID tables,
 *   syncpt protection), vm 0x13e40000 (channels at +id*0x100, syncpt
 *   regs at +0x6400/+0x8080/... -- host1x08 sync_offset=0).
 *
 * VIC firmware nvidia/tegra234/vic.bin is plaintext falcon ucode with a
 * bin-header-v1 (magic 0x10fe); no ACR/WPR2/signature chain is involved.
 */

#define VIC_VA        0x69000000ULL
#define H1X_COMMON_VA 0x69100000ULL
#define H1X_HV_VA     0x69110000ULL
#define H1X_VM_VA     0x69120000ULL
#define SMMU_A_VA     0x69200000ULL
#define SMMU_B_VA     0x69300000ULL

/* Spare syncpt/channel far from anything UEFI or BPMP might own. */
#define PROBE_SYNCPT  512u
#define PROBE_CHANNEL 62u

/* VIC falcon + THI registers (TRM §7.3.3, mainline falcon.h/vic.h). */
#define VIC_THI_STREAMID0       0x0030
#define VIC_THI_STREAMID1       0x0034
#define VIC_UCLASS_METHOD_OFF   0x0040
#define VIC_UCLASS_METHOD_DATA  0x0044
#define VIC_MISC_PRI_VIC_CG     0x16d0
#define VIC_TFBIF_TRANSCFG      0x2044
#define VIC_TFBIF_DBG_RDCOUNT   0x2014
#define VIC_TFBIF_DBG_WRCOUNT   0x201c
#define VIC_IRQSTAT             0x1008
#define VIC_IRQMSET             0x1010
#define VIC_IRQDEST             0x101c
#define VIC_MAILBOX0            0x1040
#define VIC_MAILBOX1            0x1044
#define VIC_ITFEN               0x1048
#define VIC_IDLESTATE           0x104c
#define VIC_CPUCTL              0x1100
#define VIC_BOOTVEC             0x1104
#define VIC_DMACTL              0x110c
#define VIC_DMATRFBASE          0x1110
#define VIC_DMATRFMOFFS         0x1114
#define VIC_DMATRFCMD           0x1118
#define VIC_DMATRFFBOFFS        0x111c

#define CPUCTL_STARTCPU         (1u << 1)
#define DMACTL_DMEM_SCRUB       (1u << 1)
#define DMACTL_IMEM_SCRUB       (1u << 2)
#define DMATRFCMD_IDLE          (1u << 1)
#define DMATRFCMD_FULL          (1u << 0)
#define DMATRFCMD_IMEM          (1u << 4)
#define DMATRFCMD_SIZE_256B     (6u << 8)
#define DMATRFCMD_DMACTX(v)     (((v) & 7u) << 12)
#define ITFEN_CTXEN             (1u << 0)
#define ITFEN_MTHDEN            (1u << 1)

/* VIC class methods (TRM Table 7.66).
 *
 * The falcon OS in vic.bin is a strict state machine (disassembly of the
 * method dispatcher at 0xacb and the main loop at 0x2c6):
 *  - SetApplicationID(1) selects the compositor app and must be sent
 *    exactly once per Execute; the app ID is cleared after every Execute.
 *    A second SetApplicationID, or any method the app does not know,
 *    makes the OS discard everything up to the next Execute and then
 *    complete that Execute as a no-op (the syncpt is still acked).
 *  - SetHistOffset (0x714) is NOT implemented by this firmware's app 1.
 *  - Nop (0x100) is accepted in every state.
 * So nothing but Nop may be sent outside a job, and a job carries only
 * the methods listed below. */
#define VIC_M_NOP                0x100
#define VIC_M_SET_APPLICATION_ID 0x200

/* host1x vm-aperture registers (hw_host1x08_vm.h). */
#define CH_DMASTART      0x00
#define CH_DMASTART_HI   0x04
#define CH_DMAPUT        0x08
#define CH_DMAPUT_HI     0x0c
#define CH_DMAGET        0x10
#define CH_DMAGET_HI     0x14
#define CH_DMAEND        0x18
#define CH_DMAEND_HI     0x1c
#define CH_DMACTRL       0x20
#define CH_CMDFIFO_STAT  0x24
#define CH_CHANNELSTAT   0x38
#define CH_CMDPROC_STOP  0x48
#define CH_TEARDOWN      0x4c
#define CH_SMMU_STREAMID 0x84
#define DMACTRL_DMASTOP    (1u << 0)
#define DMACTRL_DMAGETRST  (1u << 1)
#define DMACTRL_DMAINITGET (1u << 2)
#define CMDFIFO_STAT_EMPTY (1u << 13)

#define SYNC_CPU_INCR(x)   (0x6400u + 4u * ((x) / 32u))
#define SYNC_SYNCPT(x)     (0x8080u + 4u * (x))
#define SYNC_CH_APP(x)     (0xb090u + 4u * (x))

/* host1x hypervisor registers (hw_host1x08_hypervisor.h). */
#define HV_SYNCPT_PROT_EN      0x1724
#define HV_PROT_EN_VM          (1u << 0)
#define HV_PROT_EN_CH          (1u << 1)
#define HV_PROT_EN_APP         (1u << 2)

/* host1x common MLOCK for VIC (hw_host1x08_common.h). */
#define COMMON_VIC_MLOCK 0x4078

/* Pushbuffer opcodes (opcodes.h). */
#define OP_SETCLASS(cls, off, mask) ((0u << 28) | ((off) << 16) | ((cls) << 6) | (mask))
#define OP_INCR(off, cnt)           ((1u << 28) | ((off) << 16) | (cnt))
#define OP_NONINCR(off, cnt)        ((2u << 28) | ((off) << 16) | (cnt))
#define OP_IMM(off, val)            ((4u << 28) | ((off) << 16) | (val))
#define OP_RESTART(addr)            ((5u << 28) | ((addr) >> 4))
#define OP_SETSTREAMID(off)         ((7u << 28) | (off))
#define OP_SETPAYLOAD(p)            ((9u << 28) | (p))
#define OP_ACQUIRE_MLOCK(m)         ((14u << 28) | (0u << 24) | (m))
#define OP_RELEASE_MLOCK(m)         ((14u << 28) | (1u << 24) | (m))
#define OP_NOP                      OP_NONINCR(0, 0)

#define CLASS_HOST1X 0x01u
#define CLASS_VIC    0x5Du

/* Host1x class methods (hw_host1x08_uclass.h).  The legacy WAIT_SYNCPT
 * (0x08) only carries an 8-bit syncpt index; syncpts >= 256 need the
 * 32-bit pair LOAD_SYNCPT_PAYLOAD_32 (0x4e) + WAIT_SYNCPT_32 (0x50). */
#define HOST_M_INCR_SYNCPT            0x00
#define HOST_M_LOAD_SYNCPT_PAYLOAD_32 0x4e
#define INCR_COND_IMMEDIATE   0u
#define INCR_COND_OP_DONE     1u
#define INCR_COND_ENGINE_IDLE 4u
#define INCR_SYNCPT(cond, indx) (((cond) << 10) | (indx))
#define STREAMID_OFF_INVALID  0x1fffffu

/* VIC methods are not class registers: each one is an (offset, data)
 * pair written to the THI METHOD0/METHOD1 registers (TRM §7.3.2.1.1). */
#define THI_METHOD0 (VIC_UCLASS_METHOD_OFF / 4)

/* Tegra234 hypervisor SID protection table (mainline dev.c): register
 * base, then the offset/limit of the engine's stream-ID register window. */
static const uint32_t _hv_sid_table[][3] = {
    {0x1650, 0x90, 0x90}, {0x1730, 0x90, 0x90},   /* SE1 MMIO/ch */
    {0x1658, 0x90, 0x90}, {0x1738, 0x90, 0x90},   /* SE2 */
    {0x1660, 0x90, 0x90}, {0x1740, 0x90, 0x90},   /* SE4 */
    {0x1680, 0x800, 0x800},                       /* ISP MMIO */
    {0x1688, 0x34, 0x34}, {0x17b8, 0x30, 0x30},   /* VIC MMIO/ch */
    {0x1690, 0x34, 0x34}, {0x17c0, 0x30, 0x30},   /* NVENC */
    {0x1698, 0x34, 0x34}, {0x17c8, 0x30, 0x30},   /* NVDEC */
    {0x16a0, 0x34, 0x34}, {0x17d0, 0x30, 0x30},   /* NVJPG */
    {0x16a8, 0x30, 0x34},                         /* TSEC MMIO */
    {0x16b0, 0x34, 0x34}, {0x17a8, 0x30, 0x30},   /* NVJPG1 */
    {0x16b8, 0x800, 0x800},                       /* VI MMIO */
    {0x16c0, 0x30, 0x34},                         /* VI_THI */
    {0x16c8, 0x30, 0x34},                         /* ISP_THI */
    {0x16d8, 0x30, 0x34}, {0x17e0, 0x30, 0x34},   /* NVDLA */
    {0x16e0, 0x30, 0x34}, {0x17e8, 0x30, 0x34},   /* NVDLA1 */
    {0x16e8, 0x34, 0x34}, {0x1768, 0x30, 0x30},   /* OFA */
    {0x16f0, 0x800, 0x800},                       /* VI2 MMIO */
    {0x16f8, 0x30, 0x34},                         /* VI2_THI */
};

static volatile uint8_t* _vic;
static volatile uint8_t* _common;
static volatile uint8_t* _hv;
static volatile uint8_t* _vm;
static volatile uint8_t* _smmu_a;
static volatile uint8_t* _smmu_b;
static int _mapped;
static int _powered;
static int _h1x_ready;
static int _falcon_up;

static inline void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static int map_window(uint64_t va, uint64_t phy, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)phy, (ewokos_addr_t)size);
    return ret == (ewokos_addr_t)va ? 0 : -1;
}

static int ensure_mapped(void) {
    if (_mapped)
        return 0;
    if (map_window(VIC_VA, ORIN_VIC_PHY, ORIN_VIC_SIZE) != 0 ||
            map_window(H1X_COMMON_VA, ORIN_HOST1X_COMMON_PHY, ORIN_HOST1X_APERTURE) != 0 ||
            map_window(H1X_HV_VA, ORIN_HOST1X_HV_PHY, ORIN_HOST1X_APERTURE) != 0 ||
            map_window(H1X_VM_VA, ORIN_HOST1X_VM_PHY, ORIN_HOST1X_APERTURE) != 0 ||
            map_window(SMMU_A_VA, ORIN_SMMU_NISO1_A_PHY, ORIN_SMMU_GRO_SIZE) != 0 ||
            map_window(SMMU_B_VA, ORIN_SMMU_NISO1_B_PHY, ORIN_SMMU_GRO_SIZE) != 0) {
        klog("vic: SYS_MEM_MAP failed\n");
        return -1;
    }
    _vic = (volatile uint8_t*)VIC_VA;
    _common = (volatile uint8_t*)H1X_COMMON_VA;
    _hv = (volatile uint8_t*)H1X_HV_VA;
    _vm = (volatile uint8_t*)H1X_VM_VA;
    _smmu_a = (volatile uint8_t*)SMMU_A_VA;
    _smmu_b = (volatile uint8_t*)SMMU_B_VA;
    _mapped = 1;
    return 0;
}

int bsp_vic_power_on(void) {
    if (_powered)
        return 0;
    if (bsp_bpmp_init() != 0) {
        klog("vic: bpmp_init failed\n");
        return -1;
    }
    int r = bsp_bpmp_pg_on(ORIN_VIC_PG);
    klog("vic: pg_on(%u) = %d\n", ORIN_VIC_PG, r);
    if (r != 0)
        return -1;
    r = bsp_bpmp_clk_enable(ORIN_HOST1X_CLK);
    klog("vic: clk_enable(HOST1X %u) = %d\n", ORIN_HOST1X_CLK, r);
    if (r != 0)
        return -1;
    r = bsp_bpmp_clk_enable(ORIN_VIC_CLK);
    klog("vic: clk_enable(VIC %u) = %d\n", ORIN_VIC_CLK, r);
    if (r != 0)
        return -1;
    /* Max rate like mainline clk_set_rate(ULONG_MAX).  BPMP rates are
     * signed 64-bit, so ask it to round INT64_MAX down to the highest
     * supported rate first; failure is not fatal -- the enabled default
     * rate still runs the engine. */
    uint64_t max_hz = 0;
    r = bsp_bpmp_clk_round_rate(ORIN_VIC_CLK, (uint64_t)INT64_MAX, &max_hz);
    if (r == 0 && max_hz != 0 && max_hz <= (uint64_t)INT64_MAX)
        r = bsp_bpmp_clk_set_rate(ORIN_VIC_CLK, max_hz);
    klog("vic: clk_set_rate(VIC, %llu) = %d\n", (unsigned long long)max_hz, r);
    r = bsp_bpmp_reset_deassert(ORIN_VIC_RESET);
    klog("vic: reset_deassert(%u) = %d\n", ORIN_VIC_RESET, r);
    if (r != 0)
        return -1;
    usleep(1000);
    _powered = 1;
    return 0;
}

int bsp_vic_regs_probe(void) {
    if (bsp_vic_power_on() != 0 || ensure_mapped() != 0)
        return -1;
    uint32_t cpuctl = rd(_vic, VIC_CPUCTL);
    uint32_t idle = rd(_vic, VIC_IDLESTATE);
    uint32_t dmactl = rd(_vic, VIC_DMACTL);
    klog("vic: CCPLEX read OK CPUCTL=%08x IDLESTATE=%08x DMACTL=%08x\n",
            cpuctl, idle, dmactl);
    return 0;
}

/* Read-only SMMUv2 stream policy for one MMU-500 instance: 1 = bypass
 * (the engine DMAs physical addresses), 0 = translate, -1 = fault.
 * *reg is the register that decided (sCR0, or the matching S2CR). */
static int smmu_sid_policy(volatile uint8_t* s, uint32_t sid, uint32_t* reg) {
    uint32_t cr0 = rd(s, ORIN_SMMU_SCR0);
    *reg = cr0;
    if (cr0 & ORIN_SMMU_SCR0_CLIENTPD)
        return 1;
    uint32_t n = rd(s, ORIN_SMMU_IDR0) & 0xffu;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t smr = rd(s, ORIN_SMMU_SMR(i));
        if (!(smr & ORIN_SMMU_SMR_VALID))
            continue;
        uint32_t mask = (smr >> 16) & 0x7fffu;
        if (((sid ^ smr) & ~mask & 0x7fffu) != 0)
            continue;
        uint32_t s2cr = rd(s, ORIN_SMMU_S2CR(i));
        uint32_t type = ORIN_SMMU_S2CR_TYPE(s2cr);
        *reg = s2cr;
        return type == 1u ? 1 : (type == 0u ? 0 : -1);
    }
    return (cr0 & ORIN_SMMU_SCR0_USFCFG) ? -1 : 1;
}

static int smmu_check_sid(const char* name, uint32_t sid) {
    uint32_t ra = 0, rb = 0;
    int a = smmu_sid_policy(_smmu_a, sid, &ra);
    int b = smmu_sid_policy(_smmu_b, sid, &rb);
    klog("vic: smmu_niso1 %s SID=%02x policy a=%d(%08x) b=%d(%08x)"
            " (1=bypass 0=translate -1=fault)\n", name, sid, a, ra, b, rb);
    return (a == 1 && b == 1) ? 0 : -1;
}

int bsp_vic_host1x_init(void) {
    if (bsp_vic_power_on() != 0 || ensure_mapped() != 0)
        return -1;
    if (_h1x_ready)
        return 0;

    /* All buffers are handed to host1x and VIC as physical addresses, so
     * both stream IDs must bypass the SMMU.  Anything else would raise
     * context faults (or misroute the DMA): stop before the first fetch. */
    int vic_bypass = smmu_check_sid("VIC", ORIN_VIC_SID);
    int h1x_bypass = smmu_check_sid("HOST1X", ORIN_HOST1X_SID);
    if (vic_bypass != 0 || h1x_bypass != 0) {
        klog("vic: SMMU does not bypass the VIC/host1x streams - refusing DMA\n");
        return -1;
    }

    /* Hypervisor VM tables: route engine SID windows, allow all SIDs and
     * classes to all VMs, engine MMIO originates from VM1 (CCPLEX).
     * Same programming as mainline host1x_setup_virtualization_tables. */
    for (uint32_t i = 0; i < sizeof(_hv_sid_table) / sizeof(_hv_sid_table[0]); i++) {
        wr(_hv, _hv_sid_table[i][0], _hv_sid_table[i][1]);
        wr(_hv, _hv_sid_table[i][0] + 4, _hv_sid_table[i][2]);
    }
    for (uint32_t i = 0; i < 128; i++)
        wr(_hv, 0x1004 + 4 * i, 0xff);
    for (uint32_t i = 0; i < 25; i++)
        wr(_hv, 0x1404 + 4 * i, 0xff);
    for (uint32_t i = 0; i < 25; i++)
        wr(_hv, 0x1504 + 4 * i, 0x1);
    barrier();

    uint32_t prot = rd(_hv, HV_SYNCPT_PROT_EN);
    klog("vic: host1x HV SYNCPT_PROT_EN=%08x (vm=%u ch=%u app=%u)\n",
            prot, prot & 1u, (prot >> 1) & 1u, (prot >> 2) & 1u);
    /* Channel-based protection only, like mainline: our syncpt is
     * assigned to our channel below; CPU increments stay allowed. */
    wr(_hv, HV_SYNCPT_PROT_EN, HV_PROT_EN_CH);
    wr(_vm, SYNC_CH_APP(PROBE_SYNCPT), PROBE_CHANNEL << 8);
    /* Stream ID the channel fetches its pushbuffer with (mainline
     * host1x_channel_set_streamid). */
    wr(_vm, PROBE_CHANNEL * 0x100 + CH_SMMU_STREAMID, ORIN_HOST1X_SID);
    barrier();

    uint32_t mlock = rd(_common, COMMON_VIC_MLOCK);
    uint32_t chstat = rd(_vm, PROBE_CHANNEL * 0x100 + CH_CHANNELSTAT);
    klog("vic: VIC_MLOCK=%08x ch%u CHANNELSTAT=%08x\n",
            mlock, PROBE_CHANNEL, chstat);
    _h1x_ready = 1;
    return 0;
}

int bsp_vic_syncpt_test(void) {
    if (bsp_vic_host1x_init() != 0)
        return -1;
    uint32_t before = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    wr(_vm, SYNC_CPU_INCR(PROBE_SYNCPT), 1u << (PROBE_SYNCPT % 32));
    barrier();
    uint32_t after = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    klog("vic: syncpt%u CPU_INCR %u -> %u\n", PROBE_SYNCPT, before, after);
    if (after != before + 1) {
        klog("vic: syncpt CPU_INCR FAILED\n");
        return -1;
    }
    return 0;
}

/* A stalled submit leaves the command processor blocked, possibly while
 * holding the VIC MLOCK.  Same recovery as mainline cdma_freeze +
 * timeout_release_mlock: stop, tear the channel down, force the lock. */
static void ch_recover(void) {
    volatile uint8_t* ch = _vm + PROBE_CHANNEL * 0x100;
    wr(ch, CH_CMDPROC_STOP, 1);
    wr(ch, CH_DMACTRL, DMACTRL_DMASTOP);
    wr(ch, CH_TEARDOWN, 1);
    wr(_common, COMMON_VIC_MLOCK, 0);
    barrier();
    wr(ch, CH_CMDPROC_STOP, 0);
}

/* One-shot pushbuffer submit on the probe channel.  pb_size is the usable
 * buffer size (allocation minus the trailing restart word).  PUT/GET/END
 * are offsets relative to DMASTART.  DMAINITGET loads GET from PUT, so
 * the channel is started idle at offset 0 and PUT is advanced afterwards
 * (mainline cdma_start + cdma_flush) -- loading the final PUT first would
 * leave GET == PUT with nothing to execute.
 * Returns 0 once the syncpt reached sp_final and the command FIFO
 * drained.  verbose=0 keeps the per-job klog quiet (runtime 2D ops). */
static int ch_submit(uint32_t* pb, uint32_t words, uint32_t pb_size,
        uint64_t pb_phy, uint32_t sp_final, int verbose) {
    volatile uint8_t* ch = _vm + PROBE_CHANNEL * 0x100;
    pb[pb_size / 4] = OP_RESTART(0);
    barrier();

    wr(ch, CH_DMACTRL, DMACTRL_DMASTOP);
    wr(ch, CH_DMASTART, (uint32_t)pb_phy);
    wr(ch, CH_DMASTART_HI, (uint32_t)(pb_phy >> 32));
    wr(ch, CH_DMAPUT, 0);
    wr(ch, CH_DMAPUT_HI, 0);
    wr(ch, CH_DMAEND, pb_size + 4);
    wr(ch, CH_DMAEND_HI, 0);
    wr(ch, CH_DMACTRL, DMACTRL_DMASTOP | DMACTRL_DMAGETRST | DMACTRL_DMAINITGET);
    barrier();
    wr(ch, CH_DMACTRL, 0);
    wr(ch, CH_DMAPUT, words * 4);
    barrier();

    /* Short jobs retire within microseconds: spin first, then sleep. */
    uint32_t sp = 0, stat = 0;
    int done = 0;
    for (uint32_t i = 0; i < 24000 && !done; i++) {
        sp = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
        stat = rd(ch, CH_CMDFIFO_STAT);
        done = sp == sp_final && (stat & CMDFIFO_STAT_EMPTY) != 0;
        if (!done && i >= 4000)
            usleep(100);
    }
    uint32_t get = rd(ch, CH_DMAGET);
    if (verbose || !done)
        klog("vic: ch%u DMAGET=%x (put %x) CMDFIFO_STAT=%08x syncpt=%u (want %u)\n",
                PROBE_CHANNEL, get, words * 4, stat, sp, sp_final);
    if (!done) {
        klog("vic: channel submit TIMEOUT/STALL\n");
        ch_recover();
        return -1;
    }
    wr(ch, CH_DMACTRL, DMACTRL_DMASTOP);
    return 0;
}

/* Host1x-class wait for PROBE_SYNCPT >= thresh; leaves the channel in
 * the host1x class (mainline submit_wait, HOST1X_HW >= 2). */
static uint32_t* pb_wait(uint32_t* p, uint32_t thresh) {
    *p++ = OP_SETCLASS(CLASS_HOST1X, HOST_M_LOAD_SYNCPT_PAYLOAD_32, 0x5u);
    *p++ = thresh;
    *p++ = PROBE_SYNCPT;
    *p++ = OP_NOP;
    return p;
}

static uint32_t* pb_idle_fence(uint32_t* p, uint32_t thresh) {
    *p++ = OP_NONINCR(HOST_M_INCR_SYNCPT, 1);
    *p++ = INCR_SYNCPT(INCR_COND_ENGINE_IDLE, PROBE_SYNCPT);
    return pb_wait(p, thresh);
}

/* Job framing of mainline channel_program_cdma (HOST1X_HW >= 6): take
 * the VIC MLOCK, make sure the engine is idle before switching it to the
 * real stream ID, and again before the lock is released.  The two idle
 * fences advance the syncpt by one each: base+1 on open, sp_final on
 * close. */
static uint32_t* pb_job_open(uint32_t* p, uint32_t sp_base) {
    *p++ = OP_ACQUIRE_MLOCK(CLASS_VIC);
    *p++ = OP_SETCLASS(CLASS_VIC, 0, 0);
    *p++ = OP_SETPAYLOAD(0);
    *p++ = OP_SETSTREAMID(VIC_THI_STREAMID0 / 4);
    p = pb_idle_fence(p, sp_base + 1);
    *p++ = OP_SETCLASS(CLASS_VIC, 0, 0);
    *p++ = OP_SETPAYLOAD(ORIN_VIC_SID);
    *p++ = OP_SETSTREAMID(VIC_THI_STREAMID0 / 4);
    *p++ = OP_NOP;
    return p;
}

static uint32_t* pb_job_close(uint32_t* p, uint32_t sp_final) {
    p = pb_idle_fence(p, sp_final);
    *p++ = OP_NOP;
    *p++ = OP_RELEASE_MLOCK(CLASS_VIC);
    return p;
}

static uint32_t* pb_method(uint32_t* p, uint32_t method, uint32_t data) {
    *p++ = OP_INCR(THI_METHOD0, 2);
    *p++ = method >> 2;
    *p++ = data;
    return p;
}

int bsp_vic_channel_test(void) {
    if (bsp_vic_syncpt_test() != 0)
        return -1;
    ewokos_addr_t va = dma_alloc(4096);
    if (va == 0) {
        klog("vic: dma_alloc pushbuffer failed\n");
        return -1;
    }
    uint64_t phy = dma_phy_addr(va);
    uint32_t* pb = (uint32_t*)va;
    memset(pb, 0, 4096);

    /* Stand-alone host1x-class framing of mainline's prefence block:
     * host1x MLOCK, invalid stream-ID register. */
    uint32_t before = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    uint32_t* p = pb;
    *p++ = OP_ACQUIRE_MLOCK(CLASS_HOST1X);
    *p++ = OP_SETCLASS(CLASS_HOST1X, 0, 0);
    *p++ = OP_SETPAYLOAD(0);
    *p++ = OP_SETSTREAMID(STREAMID_OFF_INVALID);
    *p++ = OP_NONINCR(HOST_M_INCR_SYNCPT, 1);
    *p++ = INCR_SYNCPT(INCR_COND_IMMEDIATE, PROBE_SYNCPT);
    *p++ = OP_NOP;
    *p++ = OP_RELEASE_MLOCK(CLASS_HOST1X);
    int r = ch_submit(pb, (uint32_t)(p - pb), 4092, phy, before + 1, 1);
    uint32_t after = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    klog("vic: channel host1x-class syncpt%u %u -> %u (want +1)\n",
            PROBE_SYNCPT, before, after);
    dma_free(va);
    return r == 0 ? 0 : -1;
}

/* Falcon DMA copy of one 256B chunk.  src_off = offset inside the fw
 * image (relative to DMATRFBASE<<8), dst_off = falcon IMEM/DMEM offset. */
static int falcon_chunk(uint32_t src_off, uint32_t dst_off, int imem) {
    uint32_t cmd = DMATRFCMD_SIZE_256B | DMATRFCMD_DMACTX(1);
    if (imem)
        cmd |= DMATRFCMD_IMEM;
    for (uint32_t i = 0; i < 1000; i++) {
        if (!(rd(_vic, VIC_DMATRFCMD) & DMATRFCMD_FULL))
            break;
        usleep(100);
        if (i == 999) {
            klog("vic: falcon DMA FULL timeout\n");
            return -1;
        }
    }
    wr(_vic, VIC_DMATRFMOFFS, dst_off);
    wr(_vic, VIC_DMATRFFBOFFS, src_off);
    wr(_vic, VIC_DMATRFCMD, cmd);
    return 0;
}

static int falcon_wait_idle(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 10; i++) {
        if (rd(_vic, VIC_IDLESTATE) == 0)
            return 0;
        usleep(100);
    }
    klog("vic: falcon IDLESTATE timeout = %08x\n", rd(_vic, VIC_IDLESTATE));
    return -1;
}

/* The falcon OS demand-loads its tables from the image for as long as it
 * runs (it latches DMATRFBASE and the DMATRFCMD context at start-up), so
 * the buffer lives for the whole session and is never returned to the
 * DMA pool. */
#define FW_BUF_BYTES 32768u
static ewokos_addr_t _fw_va;

/* TFBIF_TRANSCFG: 2-bit stream-ID selector per DMA attribute at bit 4*n
 * (0 = STREAMID0, 2 = STREAMID1).  Mainline programs 0x20 (attr1 =
 * STREAMID1 for the firmware, attr0 = STREAMID0 for job buffers); here
 * every attribute rides STREAMID1, the only one MMIO can program on
 * Tegra234 (STREAMID0 is channel-only, see _hv_sid_table).  Verified on
 * the board for the firmware load and for job config/surface DMA. */
#define VIC_TRANSCFG_ALL_SID1 0x22222222u

static void vic_dump_state(const char* tag) {
    klog("vic: [%s] mailbox0=%08x mailbox1=%08x irqstat=%08x idle=%08x cpuctl=%08x"
            " tfbif_rd=%u tfbif_wr=%u\n", tag,
            rd(_vic, VIC_MAILBOX0), rd(_vic, VIC_MAILBOX1), rd(_vic, VIC_IRQSTAT),
            rd(_vic, VIC_IDLESTATE), rd(_vic, VIC_CPUCTL),
            rd(_vic, VIC_TFBIF_DBG_RDCOUNT), rd(_vic, VIC_TFBIF_DBG_WRCOUNT));
}

int bsp_vic_falcon_boot(const char* fw_path) {
    if (_falcon_up)
        return 0;
    if (bsp_vic_channel_test() != 0)
        return -1;

    FILE* f = fopen(fw_path, "rb");
    if (f == NULL) {
        klog("vic: cannot open firmware %s\n", fw_path);
        return -1;
    }
    if (_fw_va == 0)
        _fw_va = dma_alloc(FW_BUF_BYTES);
    if (_fw_va == 0) {
        fclose(f);
        klog("vic: dma_alloc firmware failed\n");
        return -1;
    }
    uint8_t* fw = (uint8_t*)_fw_va;
    memset(fw, 0, FW_BUF_BYTES);
    uint32_t fw_size = (uint32_t)fread(fw, 1, FW_BUF_BYTES, f);
    fclose(f);
    uint64_t fw_phy = dma_phy_addr(_fw_va);
    klog("vic: firmware %s size=%u phy=%llx\n", fw_path,
            fw_size, (unsigned long long)fw_phy);

    uint32_t* hdr = (uint32_t*)fw;
    if (fw_size < 0x200 || hdr[0] != 0x10fe || hdr[1] != 1 ||
            hdr[2] > fw_size) {
        klog("vic: bad fw header magic=%x ver=%u size=%x\n",
                hdr[0], hdr[1], hdr[2]);
        return -1;
    }
    uint32_t os_hdr_off = hdr[3];
    uint32_t os_data_off = hdr[4];
    if ((os_hdr_off & 3u) != 0 || os_hdr_off > fw_size - 16u ||
            (os_data_off & 0xffu) != 0 || os_data_off >= fw_size) {
        klog("vic: bad fw layout os_hdr=%x os_data=%x\n", os_hdr_off, os_data_off);
        return -1;
    }
    uint32_t* os = (uint32_t*)(fw + os_hdr_off);
    uint32_t code_off = os[0], code_size = os[1];
    uint32_t data_off = os[2], data_size = os[3];
    uint32_t fce_marker = hdr[7];
    klog("vic: fw os_data_off=%x code=%x+%x data=%x+%x fce_marker=%08x\n",
            os_data_off, code_off, code_size, data_off, data_size, fce_marker);
    /* Segments load in whole 256B chunks, so the rounded-up ends must
     * stay inside the (zero padded) DMA buffer. */
    uint64_t code_end = (uint64_t)os_data_off + code_off +
            (((uint64_t)code_size + 255u) & ~255ull);
    uint64_t data_end = (uint64_t)os_data_off + data_off +
            (((uint64_t)data_size + 255u) & ~255ull);
    if (code_size == 0 || code_end > FW_BUF_BYTES || data_end > FW_BUF_BYTES) {
        klog("vic: fw segments exceed the image\n");
        return -1;
    }
    if (fce_marker != 0 && fce_marker != 0xa5a5a5a5) {
        /* Old firmware needs the FCE ucode setup methods (mainline
         * vic_boot); the shipped tegra234 vic.bin does not. */
        klog("vic: old-style FCE firmware, setup path not implemented\n");
        return -1;
    }

    /* Clean falcon state (UEFI may have left anything behind). */
    bsp_bpmp_reset_assert(ORIN_VIC_RESET);
    usleep(1000);
    bsp_bpmp_reset_deassert(ORIN_VIC_RESET);
    usleep(1000);

    /* Mainline vic_boot: stream IDs, clock gating, then falcon_boot. */
    wr(_vic, VIC_TFBIF_TRANSCFG, VIC_TRANSCFG_ALL_SID1);
    wr(_vic, VIC_THI_STREAMID0, ORIN_VIC_SID);
    wr(_vic, VIC_THI_STREAMID1, ORIN_VIC_SID);
    /* Clock gating: idle count 4, idle CG enable, wakeup count 4. */
    wr(_vic, VIC_MISC_PRI_VIC_CG, (4u << 0) | (1u << 6) | (4u << 16));
    barrier();
    klog("vic: transcfg=%08x streamid0=%08x streamid1=%08x (sid=%02x)\n",
            rd(_vic, VIC_TFBIF_TRANSCFG), rd(_vic, VIC_THI_STREAMID0),
            rd(_vic, VIC_THI_STREAMID1), ORIN_VIC_SID);

    /* Wait for IMEM/DMEM scrub after reset, then disable scrubbing. */
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t v = rd(_vic, VIC_DMACTL);
        if (!(v & (DMACTL_IMEM_SCRUB | DMACTL_DMEM_SCRUB)))
            break;
        usleep(100);
        if (i == 999) {
            klog("vic: DMACTL scrub timeout = %08x\n", v);
            return -1;
        }
    }
    wr(_vic, VIC_DMACTL, 0);
    wr(_vic, VIC_DMATRFBASE, (uint32_t)((fw_phy + os_data_off) >> 8));

    for (uint32_t off = 0; off < data_size; off += 256) {
        if (falcon_chunk(data_off + off, off, 0) != 0)
            return -1;
    }
    for (uint32_t off = 0; off < code_size; off += 256) {
        if (falcon_chunk(code_off + off, off, 1) != 0)
            return -1;
    }
    for (uint32_t i = 0; i < 1000; i++) {
        if (rd(_vic, VIC_DMATRFCMD) & DMATRFCMD_IDLE)
            break;
        usleep(100);
        if (i == 999) {
            klog("vic: falcon DMA idle timeout\n");
            return -1;
        }
    }

    wr(_vic, VIC_IRQMSET, 0xff00u | (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4) | (1u << 1));
    wr(_vic, VIC_IRQDEST, 0xff00u | (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4));
    wr(_vic, VIC_ITFEN, ITFEN_CTXEN | ITFEN_MTHDEN);
    wr(_vic, VIC_BOOTVEC, 0);
    barrier();
    wr(_vic, VIC_CPUCTL, CPUCTL_STARTCPU);
    if (falcon_wait_idle(100) != 0) {
        klog("vic: falcon boot FAILED\n");
        vic_dump_state("boot");
        return -1;
    }

    /* Liveness through the MMIO method interface.  Only Nop is legal
     * outside a job (see the method notes above). */
    wr(_vic, VIC_UCLASS_METHOD_OFF, VIC_M_NOP >> 2);
    wr(_vic, VIC_UCLASS_METHOD_DATA, 0);
    barrier();
    if (falcon_wait_idle(100) != 0) {
        klog("vic: falcon MMIO method FAILED\n");
        vic_dump_state("nop");
        return -1;
    }
    klog("vic: falcon booted, MMIO Nop consumed, mailbox0=%08x\n",
            rd(_vic, VIC_MAILBOX0));
    _falcon_up = 1;
    return 0;
}

int bsp_vic_method_test(void) {
    if (bsp_vic_falcon_boot("/drivers/orin.nano/firmware/vic.bin") != 0)
        return -1;
    ewokos_addr_t va = dma_alloc(4096);
    if (va == 0) {
        klog("vic: dma_alloc pushbuffer failed\n");
        return -1;
    }
    uint64_t phy = dma_phy_addr(va);
    uint32_t* pb = (uint32_t*)va;
    memset(pb, 0, 4096);

    /* One Nop through the full job framing: the two ENGINE_IDLE fences
     * are the only syncpt increments. */
    uint32_t before = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    uint32_t final = before + 2;
    uint32_t* p = pb_job_open(pb, before);
    p = pb_method(p, VIC_M_NOP, 0);
    *p++ = OP_NOP;
    p = pb_job_close(p, final);
    int r = ch_submit(pb, (uint32_t)(p - pb), 4092, phy, final, 1);
    uint32_t after = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    uint32_t mlock = rd(_common, COMMON_VIC_MLOCK);
    klog("vic: VIC-class method syncpt%u %u -> %u (want %u) VIC_MLOCK=%08x\n",
            PROBE_SYNCPT, before, after, final, mlock);
    dma_free(va);
    if (r != 0) {
        klog("vic: VIC method routing FAILED\n");
        return -1;
    }
    klog("vic: M1 PASS -- VIC falcon alive, host1x channel + method routing OK\n");
    return 0;
}

/* ================= milestone 2: ConfigStruct 2D ops =================
 *
 * TRM DP-10508-002 v1.2p §7.3.2, cross-checked against the two producers
 * that are proven on hardware (T124..T194): libdrm tests/tegra/vic42.c
 * and vaapi-tegra-driver engines/vic.cpp.  Pitch-linear ARGB8888
 * surfaces addressed by physical base (SMMU bypass), no scaling, no
 * blending:
 *  - fill = background colour inside the target rect, no slot enabled
 *    (libdrm vic42_fill);
 *  - blit/rotate = one enabled slot copied 1:1, output flip/transpose.
 * The kernel maps contiguous shm canvases, the DMA pool and the scan-out
 * framebuffer uncached, so no cache maintenance is needed around a job.
 *
 * Encodings: ConfigStruct 2960B / 16 slots (libdrm vic42, Table 7.58/7.64),
 * offset methods carry
 * phy>>8 (address bits [39:8]), pitch padded to 256B, rects inclusive
 * (Right = x+w-1), SourceRect in U14.16 with Right/Bottom = (end-1)<<16
 * so the scale ratio is exactly 1:1, SoftClampHigh = 1023 (0 would clamp
 * every component), PlanarAlpha = 1023 (10 bits at slot bit 160) so the
 * colour is not attenuated.  Rotation bits per Table 7.38 + rule 5
 * (flips first, then transpose): clockwise 90 = Transpose+FlipY.
 * Surface heights must be multiples of 4 lines: the surface cache works
 * on 64Bx4 blocks and the padded height may not reach past the buffer.
 *
 * What the documentation leaves open is arbitrated by the readback
 * selftest in bsp_vic_2d_enable on every start.  Measured on the Orin
 * Nano with nvidia/tegra234/vic.bin:
 *  - pixel format enum 32 stores 0xAARRGGBB words, chroma size 16383;
 *  - AlphaFillMode COMPOSITED + ConstantAlpha 0 copies all four bytes,
 *    SOURCE_STREAM + ConstantAlpha 1 copies colour and writes alpha ff
 *    (with ConstantAlpha 0 the stream modes premultiply the colour);
 *  - the rotation mapping, odd rects and heights off the 4-line grid
 *    are exact; a source pitch off the 256B grid is not.
 */

/* VIC class methods, byte offsets (Table 7.66). */
#define VIC_M_EXECUTE             0x300
#define VIC_M_SET_CONTROL_PARAMS  0x704
#define VIC_M_SET_CONFIG_STRUCT   0x708
#define VIC_M_SET_FILTER_STRUCT   0x70c
#define VIC_M_SET_OUT_LUMA        0x720
#define VIC_M_SET_NOTIFIER_OUT    0x830
#define VIC_M_SET_SRC0_SLOT0_LUMA 0x1200
#define VIC_EXECUTE_AWAKEN        (1u << 8)   /* notify: acks OP_DONE */

/* The falcon OS writes a 16-byte notifier {time_lo, time_hi, info32,
 * info16, status16} to the SetStatusNotifierOutputOffset buffer: 0x2b
 * when the Execute starts, the result when it ends (0 = done).  Result
 * codes (firmware disassembly): 0x19 bad context ID, 0x1a ConfigStruct
 * size != 185 units, 0x1b timeout, 0x1c..0x21 pipeline error interrupt
 * bits 0..5, 0x22 bad application ID, 0x24 firmware table load failed,
 * 0x25 Execute without the notify bit, 0x26 unknown method in the job,
 * 0x27 repeated SetApplicationID, 0x28 notifier table full. */
#define NTF_STATUS_OFF   14
#define NTF_UNWRITTEN    0xffffu
#define NTF_DONE         0u

/* ConfigStruct byte offsets (128-bit units; slot = 1408 bits = 176B).
 * The firmware insists on the full 16-slot struct: SetControlParams
 * size = 2960/16 = 185 (anything else ends the job with 0x1a). */
#define CFG_BYTES   2960
#define CFG_OUT     16
#define CFG_OUTSURF 32
#define CFG_SLOT0   144
#define SL_SURF     64
#define SL_BLEND    160

/* DXVAHD_ALPHA_FILL_MODE (TRM Table 7.49). */
#define AFM_OPAQUE        0u
#define AFM_BACKGROUND    1u
#define AFM_SOURCE_STREAM 3u   /* source alpha, planar alpha not applied */
#define AFM_COMPOSITED    4u   /* explicit blend factors */
#define AFM_SOURCE_ALPHA  5u   /* source alpha times planar alpha */

/* Persistent job arena + selftest canvases (64x64 ARGB, 256B pitch). */
#define ARENA_BYTES     65536
#define AR_PB           0
#define AR_PB_BYTES     4096
#define AR_CFG          4096
#define AR_NOTIFIER     8192
#define AR_SRC          12288
#define AR_DST          28672
#define AR_FILTER       45056
#define AR_FILTER_BYTES 16384
#define ST_W            64
#define ST_H            64
#define ST_PITCH        256
#define ST_REGION       16384u   /* bytes of AR_SRC / AR_DST each */
#define ST_SENTINEL     0xa5a5a5a5u

typedef struct {
    ewokos_addr_t phy;
    uint32_t pitch;
    int32_t w, h;
} vic_surf_t;

typedef struct {
    vic_surf_t out, src;
    int32_t sx, sy, sw, sh;   /* source crop */
    int32_t dx, dy;           /* landing top-left in output memory */
    int32_t degree;           /* clockwise 0/90/180/270 */
    uint32_t color;
    int is_fill;
    uint32_t afm, calpha;     /* copy jobs: alpha fill mode, ConstantAlpha */
} vic_job_t;

/* g2dd dispatches from several worker threads; the arena, the channel
 * and the syncpt are one shared resource. */
static pthread_mutex_t _vic_lock = PTHREAD_MUTEX_INITIALIZER;
static ewokos_addr_t _arena;
static uint64_t _arena_phy;
static int _2d_state;         /* 0 untried, 1 passed, -1 failed */
static int _2d_dead;          /* submitted job never retired: stay off */
static int _rot_ok;
static uint32_t _vic_fmt = 32;
static uint32_t _vic_chroma = 16383;
static uint32_t _vic_hz;
/* Copy configurations latched by the selftest: one that reproduces all
 * four bytes, one that reproduces colour only (for alpha-less targets
 * such as the scan-out framebuffer). */
static int _copy_ok, _xrgb_ok;
static uint32_t _copy_afm, _copy_calpha, _xrgb_afm, _xrgb_calpha;
#define VIC_MAX_REJECTS 8u
static uint32_t _rejects;
/* Geometry latched by the selftest: odd rect coordinates/sizes, and
 * surface heights that are not a multiple of 4 lines. */
static int _odd_ok, _h_free;

/* Little-endian bit pack into a zeroed ConfigStruct region. */
static void bits(uint8_t* base, uint32_t off, uint32_t width, uint32_t val) {
    for (uint32_t i = 0; i < width; i++) {
        uint32_t b = off + i;
        uint8_t m = (uint8_t)(1u << (b & 7u));
        if ((val >> i) & 1u)
            base[b >> 3] |= m;
        else
            base[b >> 3] &= (uint8_t)~m;
    }
}

/* 14-bit rect at bit offsets off/off+16/off+32/off+48, inclusive edges. */
static void rect14(uint8_t* base, uint32_t off,
        int32_t x, int32_t y, int32_t w, int32_t h) {
    bits(base, off, 14, (uint32_t)x);
    bits(base, off + 16, 14, (uint32_t)(x + w - 1));
    bits(base, off + 32, 14, (uint32_t)y);
    bits(base, off + 48, 14, (uint32_t)(y + h - 1));
}

/* OutputSurfaceConfig / SlotSurfaceConfig share this shape; only the
 * slot variant carries CacheWidth.  With transpose the programmed dims
 * are pre-transform: the hardware swaps them (and LumaWidth/Height) to
 * describe the memory surface (§7.3.1.3 transformation rule 1). */
static void surf_cfg(uint8_t* b, const vic_surf_t* s, uint32_t transpose,
        int is_slot) {
    uint32_t pitch_px = s->pitch / 4u;
    uint32_t w = transpose ? (uint32_t)s->h : (uint32_t)s->w;
    uint32_t h = transpose ? (uint32_t)s->w : (uint32_t)s->h;
    uint32_t lw = transpose ? (uint32_t)s->h : pitch_px;
    uint32_t lh = transpose ? pitch_px : (uint32_t)s->h;
    bits(b, 0, 7, _vic_fmt);
    bits(b, 11, 4, 0);                 /* BLK_KIND_PITCH */
    if (is_slot)
        bits(b, 19, 3, 2);             /* CacheWidth 64Bx4 (PL recommendation) */
    bits(b, 32, 14, w - 1);
    bits(b, 46, 14, h - 1);
    bits(b, 64, 14, lw - 1);
    bits(b, 78, 14, lh - 1);
    bits(b, 96, 14, _vic_chroma);
    bits(b, 110, 14, _vic_chroma);
}

static void job_transform(int32_t degree, uint32_t* fx, uint32_t* fy,
        uint32_t* t) {
    *fx = *fy = *t = 0;
    switch (degree) {
    case 90:  *t = 1; *fy = 1; break;
    case 180: *fy = 1; *fx = 1; break;
    case 270: *t = 1; *fx = 1; break;
    default: break;
    }
}

/* Dest/target rects live in pre-transform space: the inverse of the
 * flip-then-transpose map applied to the memory landing rect.  The slot
 * keeps the unrotated (sw,sh) crop so the scaler stays 1:1. */
static void job_pre_rect(const vic_job_t* j, int32_t* px, int32_t* py) {
    switch (j->degree) {
    case 90:  *px = j->dy; *py = j->out.w - j->sh - j->dx; break;
    case 180: *px = j->out.w - j->sw - j->dx;
              *py = j->out.h - j->sh - j->dy; break;
    case 270: *px = j->out.h - j->sw - j->dy; *py = j->dx; break;
    default:  *px = j->dx; *py = j->dy; break;
    }
}

static void build_config(uint8_t* c, const vic_job_t* j) {
    uint32_t fx, fy, t;
    int32_t px, py;
    job_transform(j->degree, &fx, &fy, &t);
    job_pre_rect(j, &px, &py);

    /* PipeConfig stays zero, as in the reference producers. */
    uint8_t* o = c + CFG_OUT;
    if (j->is_fill) {
        /* 8-bit components sit in the top of the 10-bit fields, so the
         * 8-bit output reproduces them whether it truncates or rounds. */
        uint32_t a = (j->color >> 24) & 0xffu;
        bits(o, 0, 3, a == 0xffu ? AFM_OPAQUE : AFM_BACKGROUND);
        bits(o, 6, 10, a << 2);
        bits(o, 16, 10, ((j->color >> 16) & 0xffu) << 2);
        bits(o, 26, 10, ((j->color >> 8) & 0xffu) << 2);
        bits(o, 36, 10, (j->color & 0xffu) << 2);
    } else {
        bits(o, 0, 3, j->afm);
        bits(o, 3, 3, 0);              /* AlphaFillSlot */
    }
    bits(o, 48, 1, fx);
    bits(o, 49, 1, fy);
    bits(o, 50, 1, t);
    rect14(o, 64, px, py, j->sw, j->sh);

    surf_cfg(c + CFG_OUTSURF, &j->out, t, 0);
    /* outColorMatrixStruct / clear rects / per-slot matrices / luma key
     * stay zero: matrix_enable=0, LumaKeyEnabled=0. */
    if (j->is_fill)
        return;

    uint8_t* s = c + CFG_SLOT0;
    bits(s, 0, 1, 1);                  /* SlotEnable */
    bits(s, 8, 1, 1);                  /* CurrentFieldEnable */
    bits(s, 138, 10, 1023);            /* SoftClampHigh */
    bits(s, 160, 10, 1023);            /* PlanarAlpha (Table 7.64) */
    bits(s, 170, 1, j->calpha);        /* ConstantAlpha */
    bits(s, 256, 30, (uint32_t)j->sx << 16);
    bits(s, 288, 30, (uint32_t)(j->sx + j->sw - 1) << 16);
    bits(s, 320, 30, (uint32_t)j->sy << 16);
    bits(s, 352, 30, (uint32_t)(j->sy + j->sh - 1) << 16);
    rect14(s, 384, px, py, j->sw, j->sh);
    surf_cfg(s + SL_SURF, &j->src, 0, 1);
    if (j->afm == AFM_COMPOSITED) {
        /* out = K1 * src + 0 * dst for colour and alpha, K1 = 1.0. */
        uint8_t* b = s + SL_BLEND;
        bits(b, 0, 10, 1023);          /* AlphaK1 */
        bits(b, 32, 3, 0);             /* SrcFactC = K1 */
        bits(b, 36, 3, 5);             /* DstFactC = ZERO */
        bits(b, 40, 3, 0);             /* SrcFactA = K1 */
        bits(b, 44, 3, 2);             /* DstFactA = ZERO */
    }
}

/* Build + submit one job (caller holds _vic_lock).  0 = done, -1 = the
 * firmware rejected the job (it has retired; callers may fall back to
 * the CPU), -3 = submitted but not retired (terminal: latches the engine
 * off, callers must never CPU-replay).  The syncpt advances by 3: idle
 * fence on open, OP_DONE of the Execute, idle fence on close. */
static int vic_job_run(const vic_job_t* j) {
    if (_2d_dead || _arena == 0)
        return -1;
    uint8_t* cfg = (uint8_t*)(_arena + AR_CFG);
    uint32_t* pb = (uint32_t*)(_arena + AR_PB);
    volatile uint16_t* ntf = (volatile uint16_t*)(_arena + AR_NOTIFIER);
    memset(cfg, 0, CFG_BYTES);
    build_config(cfg, j);
    ntf[NTF_STATUS_OFF / 2] = NTF_UNWRITTEN;

    uint32_t before = rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT));
    uint32_t final = before + 3;

    uint32_t* p = pb_job_open(pb, before);
    p = pb_method(p, VIC_M_SET_APPLICATION_ID, 1);
    p = pb_method(p, VIC_M_SET_NOTIFIER_OUT,
            (uint32_t)((_arena_phy + AR_NOTIFIER) >> 8));
    p = pb_method(p, VIC_M_SET_CONTROL_PARAMS, (CFG_BYTES / 16u) << 16);
    p = pb_method(p, VIC_M_SET_CONFIG_STRUCT,
            (uint32_t)((_arena_phy + AR_CFG) >> 8));
    p = pb_method(p, VIC_M_SET_FILTER_STRUCT,
            (uint32_t)((_arena_phy + AR_FILTER) >> 8));
    p = pb_method(p, VIC_M_SET_OUT_LUMA, (uint32_t)(j->out.phy >> 8));
    if (!j->is_fill)
        p = pb_method(p, VIC_M_SET_SRC0_SLOT0_LUMA, (uint32_t)(j->src.phy >> 8));
    p = pb_method(p, VIC_M_EXECUTE, VIC_EXECUTE_AWAKEN);
    *p++ = OP_NONINCR(HOST_M_INCR_SYNCPT, 1);
    *p++ = INCR_SYNCPT(INCR_COND_OP_DONE, PROBE_SYNCPT);
    if ((p - pb) & 1)
        *p++ = OP_NOP;
    p = pb_job_close(p, final);

    if (ch_submit(pb, (uint32_t)(p - pb), AR_PB_BYTES - 4,
            _arena_phy + AR_PB, final, 0) != 0) {
        _2d_dead = 1;
        klog("vic: job NOT retired (syncpt %u want %u) - latched off\n",
                rd(_vm, SYNC_SYNCPT(PROBE_SYNCPT)), final);
        vic_dump_state("stall");
        return -3;
    }
    uint32_t status = ntf[NTF_STATUS_OFF / 2];
    if (status != NTF_DONE) {
        klog("vic: job rejected, firmware status=%04x\n", status);
        vic_dump_state("job");
        /* The job has retired, so the caller may redo it on the CPU; a
         * run of rejections in service means the engine is not usable. */
        if (_2d_state == 1 && ++_rejects >= VIC_MAX_REJECTS) {
            _2d_state = -1;
            klog("vic: %u jobs rejected in a row - 2D offload disabled\n",
                    _rejects);
        }
        return -1;
    }
    _rejects = 0;
    return 0;
}

/* The base rides in an offset method as phy>>8, and the engine fetches a
 * slot surface on a 256B pitch grid whatever LumaWidth says (measured: a
 * 320B-pitch source is read as 256B), so both are hard requirements.
 * strict additionally asks for a height that is a multiple of 4 lines. */
static int surf_ok(ewokos_addr_t phy, uint32_t pitch, int32_t w, int32_t h,
        int strict) {
    if (phy == 0 || (phy & 255u) != 0 || (pitch & 255u) != 0 ||
            w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
            pitch < (uint32_t)w * 4u || pitch / 4u > 16384)
        return 0;
    return !strict || (h & 3) == 0;
}

static int rect_even(int32_t x, int32_t y, int32_t w, int32_t h) {
    return ((x | y | w | h) & 1) == 0;
}

static int rect_in(int32_t x, int32_t y, int32_t w, int32_t h,
        int32_t sw, int32_t sh) {
    return x >= 0 && y >= 0 && w > 0 && h > 0 &&
            x <= sw - w && y <= sh - h;
}

int bsp_vic_2d_available(void) {
    return _2d_state == 1 && !_2d_dead;
}

int bsp_vic_2d_can_rotate(void) {
    return bsp_vic_2d_available() && _rot_ok;
}

uint32_t bsp_vic_2d_clock_hz(void) {
    return _vic_hz;
}

int bsp_vic_fill(ewokos_addr_t dst_phy, uint32_t dst_pitch,
        int32_t dst_w, int32_t dst_h,
        int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!bsp_vic_2d_available())
        return -1;
    if (!surf_ok(dst_phy, dst_pitch, dst_w, dst_h, !_h_free) ||
            !rect_in(x, y, w, h, dst_w, dst_h))
        return -1;
    if (!_odd_ok && !rect_even(x, y, w, h))
        return -1;
    vic_job_t j;
    memset(&j, 0, sizeof(j));
    j.out.phy = dst_phy;
    j.out.pitch = dst_pitch;
    j.out.w = dst_w;
    j.out.h = dst_h;
    j.sw = w;
    j.sh = h;
    j.dx = x;
    j.dy = y;
    j.color = color;
    j.is_fill = 1;
    pthread_mutex_lock(&_vic_lock);
    int r = vic_job_run(&j);
    pthread_mutex_unlock(&_vic_lock);
    return r;
}

static int vic_copy(ewokos_addr_t src_phy, uint32_t src_pitch,
        int32_t src_w, int32_t src_h,
        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
        ewokos_addr_t dst_phy, uint32_t dst_pitch,
        int32_t dst_w, int32_t dst_h,
        int32_t dx, int32_t dy, int32_t degree,
        uint32_t afm, uint32_t calpha) {
    if (degree != 0 && degree != 90 && degree != 180 && degree != 270)
        return -1;
    if (degree != 0 && !_rot_ok)
        return -1;
    /* rotation is only proven on the strict geometry */
    int strict = degree != 0 || !_h_free;
    if (!surf_ok(src_phy, src_pitch, src_w, src_h, strict) ||
            !surf_ok(dst_phy, dst_pitch, dst_w, dst_h, strict) ||
            !rect_in(sx, sy, sw, sh, src_w, src_h))
        return -1;
    int32_t rw = (degree == 90 || degree == 270) ? sh : sw;
    int32_t rh = (degree == 90 || degree == 270) ? sw : sh;
    if (!rect_in(dx, dy, rw, rh, dst_w, dst_h))
        return -1;
    if ((degree != 0 || !_odd_ok) &&
            (!rect_even(sx, sy, sw, sh) || !rect_even(dx, dy, rw, rh)))
        return -1;
    /* The engine reads and writes concurrently: no overlapping surfaces. */
    uint64_t s0 = src_phy, s1 = s0 + (uint64_t)src_pitch * (uint32_t)src_h;
    uint64_t d0 = dst_phy, d1 = d0 + (uint64_t)dst_pitch * (uint32_t)dst_h;
    if (s0 < d1 && d0 < s1)
        return -1;
    vic_job_t j;
    memset(&j, 0, sizeof(j));
    j.src.phy = src_phy;
    j.src.pitch = src_pitch;
    j.src.w = src_w;
    j.src.h = src_h;
    j.out.phy = dst_phy;
    j.out.pitch = dst_pitch;
    j.out.w = dst_w;
    j.out.h = dst_h;
    j.sx = sx;
    j.sy = sy;
    j.sw = sw;
    j.sh = sh;
    j.dx = dx;
    j.dy = dy;
    j.degree = degree;
    j.afm = afm;
    j.calpha = calpha;
    pthread_mutex_lock(&_vic_lock);
    int r = vic_job_run(&j);
    pthread_mutex_unlock(&_vic_lock);
    return r;
}

int bsp_vic_blit(ewokos_addr_t src_phy, uint32_t src_pitch,
        int32_t src_w, int32_t src_h,
        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
        ewokos_addr_t dst_phy, uint32_t dst_pitch,
        int32_t dst_w, int32_t dst_h,
        int32_t dx, int32_t dy, int32_t degree) {
    if (!bsp_vic_2d_available() || !_copy_ok)
        return -1;
    return vic_copy(src_phy, src_pitch, src_w, src_h, sx, sy, sw, sh,
            dst_phy, dst_pitch, dst_w, dst_h, dx, dy, degree,
            _copy_afm, _copy_calpha);
}

int bsp_vic_blit_xrgb(ewokos_addr_t src_phy, uint32_t src_pitch,
        int32_t src_w, int32_t src_h,
        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
        ewokos_addr_t dst_phy, uint32_t dst_pitch,
        int32_t dst_w, int32_t dst_h,
        int32_t dx, int32_t dy, int32_t degree) {
    if (!bsp_vic_2d_available() || !_xrgb_ok)
        return -1;
    return vic_copy(src_phy, src_pitch, src_w, src_h, sx, sy, sw, sh,
            dst_phy, dst_pitch, dst_w, dst_h, dx, dy, degree,
            _xrgb_afm, _xrgb_calpha);
}

/* --- bring-up selftest: exact pixel readback of every op shape --- */

/* Every byte varies, the alpha byte included: an opaque-only pattern
 * cannot tell a verbatim copy from one that rewrites or applies alpha. */
static uint32_t st_px(int32_t x, int32_t y) {
    return ((uint32_t)(uint8_t)(x * 5 + y * 3 + 1) << 24) |
            ((uint32_t)(x * 4 + 1) << 16) |
            ((uint32_t)(y * 4 + 2) << 8) | (uint8_t)(x * 7 + y * 13);
}

static void st_src_pattern(uint32_t* src, uint32_t pitch) {
    uint32_t stride = pitch / 4u;
    for (uint32_t i = 0; i < ST_REGION / 4u; i++)
        src[i] = st_px((int32_t)(i % stride), (int32_t)(i / stride));
}

/* Whole-region check (the 16K test region, row padding and the bytes
 * past the last surface line included): outside the landing rect the
 * sentinel must survive in full; inside, either the flat fill color or
 * the rotated/blitted source crop, exact under mask.  Returns the number
 * of wrong pixels and logs the first one. */
static uint32_t st_verify(const char* tag, const uint32_t* d, uint32_t dpitch,
        int32_t rx, int32_t ry, int32_t rw, int32_t rh,
        int flat, uint32_t color,
        const uint32_t* s, uint32_t spitch, int32_t sx, int32_t sy,
        int32_t sw, int32_t sh, int32_t degree, uint32_t mask) {
    uint32_t bad = 0;
    uint32_t dstride = dpitch / 4u, sstride = spitch / 4u;
    for (uint32_t i = 0; i < ST_REGION / 4u; i++) {
        int32_t x = (int32_t)(i % dstride), y = (int32_t)(i / dstride);
        uint32_t got = d[i];
        uint32_t want = ST_SENTINEL;
        uint32_t m = 0xffffffffu;
        if (x >= rx && y >= ry && x < rx + rw && y < ry + rh) {
            m = mask;
            if (flat) {
                want = color;
            } else {
                int32_t u = x - rx, v = y - ry, fx, fy;
                switch (degree) {
                case 90:  fx = v; fy = sh - 1 - u; break;
                case 180: fx = sw - 1 - u; fy = sh - 1 - v; break;
                case 270: fx = sw - 1 - v; fy = u; break;
                default:  fx = u; fy = v; break;
                }
                want = s[(uint32_t)(sy + fy) * sstride + (uint32_t)(sx + fx)];
            }
        }
        if (((got ^ want) & m) != 0) {
            if (bad == 0)
                klog("vic: %s (%d,%d) got=%08x want=%08x mask=%08x\n",
                        tag, x, y, got, want, m);
            bad++;
        }
    }
    return bad;
}

#define ST_RGB 0x00ffffffu

/* One fill and one 1:1 copy on a 64 x h surface pair: the landing rect is
 * (x,y,rw,rh), the source crop starts at (sx,sy).  1 = both exact over
 * the whole region, 0 = not, -1 = engine stalled. */
static int st_geo(const char* tag,
        int32_t x, int32_t y, int32_t rw, int32_t rh,
        int32_t sx, int32_t sy, int32_t h, uint32_t mask) {
    const uint32_t pitch = ST_PITCH;
    const int32_t w = ST_W;
    uint32_t* src = (uint32_t*)(_arena + AR_SRC);
    uint32_t* dst = (uint32_t*)(_arena + AR_DST);
    vic_job_t j;
    memset(&j, 0, sizeof(j));
    j.out.phy = _arena_phy + AR_DST;
    j.out.pitch = pitch;
    j.out.w = w;
    j.out.h = h;
    j.sw = rw; j.sh = rh;
    j.dx = x; j.dy = y;
    j.color = 0xff556677u;
    j.is_fill = 1;
    memset(dst, 0xa5, ST_REGION);
    int r = vic_job_run(&j);
    if (r == -3)
        return -1;
    uint32_t bad_fill = r != 0 ? 0xffffffffu :
            st_verify(tag, dst, pitch, x, y, rw, rh, 1, j.color,
                    src, pitch, 0, 0, 0, 0, 0, 0xffffffffu);

    j.is_fill = 0;
    j.src.phy = _arena_phy + AR_SRC;
    j.src.pitch = pitch;
    j.src.w = w;
    j.src.h = h;
    j.sx = sx; j.sy = sy;
    j.afm = _copy_ok ? _copy_afm : _xrgb_afm;
    j.calpha = _copy_ok ? _copy_calpha : _xrgb_calpha;
    memset(dst, 0xa5, ST_REGION);
    r = vic_job_run(&j);
    uint32_t bad_copy = r != 0 ? 0xffffffffu :
            st_verify(tag, dst, pitch, x, y, rw, rh, 0, 0,
                    src, pitch, sx, sy, rw, rh, 0, mask);
    klog("vic: M2 geometry %s %dx%d rect=%d,%d %dx%d fill bad=%u copy bad=%u\n",
            tag, w, h, x, y, rw, rh, bad_fill, bad_copy);
    if (r == -3)
        return -1;
    return bad_fill == 0 && bad_copy == 0;
}

static int selftest(void) {
    uint32_t* src = (uint32_t*)(_arena + AR_SRC);
    uint32_t* dst = (uint32_t*)(_arena + AR_DST);
    st_src_pattern(src, ST_PITCH);

    vic_job_t j;
    memset(&j, 0, sizeof(j));
    j.out.phy = _arena_phy + AR_DST;
    j.out.pitch = ST_PITCH;
    j.out.w = ST_W;
    j.out.h = ST_H;

    /* Fill, calibrating the two encodings the documentation leaves open:
     * the single-plane chroma size convention and which pixel-format
     * enum stores 0xAARRGGBB words.  Proven-producer values go first. */
    static const uint32_t chromas[2] = {16383, 0};
    static const uint32_t fmts[2] = {32, 31};
    int ok = 0;
    j.sw = 32; j.sh = 24;
    j.dx = 8; j.dy = 8;
    j.color = 0xff112233u;
    j.is_fill = 1;
    for (int i = 0; i < 4 && !ok; i++) {
        _vic_chroma = chromas[i >> 1];
        _vic_fmt = fmts[i & 1];
        memset(dst, 0xa5, ST_REGION);
        int r = vic_job_run(&j);
        if (r == -3)
            return -1;
        uint32_t bad = r != 0 ? 0xffffffffu :
                st_verify("fill", dst, ST_PITCH, 8, 8, 32, 24, 1, j.color,
                        src, ST_PITCH, 0, 0, 0, 0, 0, 0xffffffffu);
        klog("vic: M2 fill chroma=%u fmt=%u job=%d bad=%u\n",
                _vic_chroma, _vic_fmt, r, bad);
        ok = bad == 0;
    }
    if (!ok)
        return -1;

    /* 1:1 copy of an asymmetric crop.  Which alpha-fill mode carries the
     * source alpha byte through unchanged is not documented precisely
     * enough to pick one offline, so every candidate is measured; the
     * first alpha-exact one serves canvas blits, the first colour-exact
     * one serves alpha-less targets. */
    static const uint32_t afms[4] = {AFM_SOURCE_STREAM, AFM_SOURCE_ALPHA,
            AFM_COMPOSITED, AFM_OPAQUE};
    j.is_fill = 0;
    j.src.phy = _arena_phy + AR_SRC;
    j.src.pitch = ST_PITCH;
    j.src.w = ST_W;
    j.src.h = ST_H;
    j.sx = 4; j.sy = 6; j.sw = 24; j.sh = 20;
    j.dx = 32; j.dy = 32;
    j.degree = 0;
    for (int i = 0; i < 8 && !_copy_ok; i++) {
        j.afm = afms[i >> 1];
        j.calpha = (i & 1) ? 0u : 1u;
        memset(dst, 0xa5, ST_REGION);
        int r = vic_job_run(&j);
        if (r == -3)
            return -1;
        uint32_t bad = 0xffffffffu, bad_rgb = 0xffffffffu;
        if (r == 0) {
            bad = st_verify("copy", dst, ST_PITCH, 32, 32, 24, 20, 0, 0,
                    src, ST_PITCH, 4, 6, 24, 20, 0, 0xffffffffu);
            bad_rgb = bad == 0 ? 0 :
                    st_verify("copy-rgb", dst, ST_PITCH, 32, 32, 24, 20, 0, 0,
                            src, ST_PITCH, 4, 6, 24, 20, 0, ST_RGB);
        }
        klog("vic: M2 copy afm=%u calpha=%u job=%d bad=%u bad_rgb=%u\n",
                j.afm, j.calpha, r, bad, bad_rgb);
        if (bad_rgb == 0 && !_xrgb_ok) {
            _xrgb_ok = 1;
            _xrgb_afm = j.afm;
            _xrgb_calpha = j.calpha;
        }
        if (bad == 0) {
            _copy_ok = 1;
            _copy_afm = j.afm;
            _copy_calpha = j.calpha;
        }
    }
    if (!_xrgb_ok)
        return 0;   /* fill only */

    /* Right-angle rotations into an offset landing rect.  A mismatch
     * only keeps rotation on the CPU; fill and blit stay accelerated. */
    static const int32_t degs[3] = {90, 180, 270};
    uint32_t mask = _copy_ok ? 0xffffffffu : ST_RGB;
    j.afm = _copy_ok ? _copy_afm : _xrgb_afm;
    j.calpha = _copy_ok ? _copy_calpha : _xrgb_calpha;
    _rot_ok = 1;
    for (int i = 0; i < 3 && _rot_ok; i++) {
        memset(dst, 0xa5, ST_REGION);
        j.degree = degs[i];
        j.dx = 8; j.dy = 4;
        int32_t rw = (j.degree == 180) ? j.sw : j.sh;
        int32_t rh = (j.degree == 180) ? j.sh : j.sw;
        int r = vic_job_run(&j);
        if (r == -3) {
            _rot_ok = 0;
            return -1;
        }
        uint32_t bad = r != 0 ? 0xffffffffu :
                st_verify("rotate", dst, ST_PITCH, 8, 4, rw, rh, 0, 0,
                        src, ST_PITCH, 4, 6, 24, 20, j.degree, mask);
        klog("vic: M2 rotate%d job=%d bad=%u\n", j.degree, r, bad);
        if (bad != 0)
            _rot_ok = 0;
    }

    /* Geometry the documentation only recommends against: odd rects and
     * heights off the 4-line grid.  Each is measured; a miss only keeps
     * that geometry on the CPU. */
    int odd = st_geo("odd", 3, 5, 21, 17, 5, 3, ST_H, mask);
    if (odd < 0)
        return -1;
    _odd_ok = odd;
    int hany = st_geo("height", 8, 41, 20, 20, 2, 41, 61, mask);
    if (hany < 0)
        return -1;
    int hodd = hany && odd ? st_geo("height-odd", 3, 40, 59, 21, 1, 40, 61, mask) : 0;
    if (hodd < 0)
        return -1;
    _h_free = hany && (!odd || hodd);
    return 0;
}

int bsp_vic_2d_enable(void) {
    pthread_mutex_lock(&_vic_lock);
    if (_2d_state == 0) {
        _2d_state = -1;
        if (bsp_vic_method_test() == 0)
            _arena = dma_alloc(ARENA_BYTES);
        if (_arena != 0) {
            _arena_phy = dma_phy_addr(_arena);
            memset((void*)_arena, 0, ARENA_BYTES);
            uint64_t rate = 0;
            if (bsp_bpmp_clk_get_rate(ORIN_VIC_CLK, &rate) == 0)
                _vic_hz = (uint32_t)rate;
            if (selftest() == 0) {
                _2d_state = 1;
                klog("vic: M2 PASS -- fill exact, copy=%d (afm=%u calpha=%u)"
                        " xrgb=%d (afm=%u calpha=%u) rotate=%d odd=%d"
                        " any-height=%d fmt=%u chroma=%u clock=%u Hz\n",
                        _copy_ok, _copy_afm, _copy_calpha,
                        _xrgb_ok, _xrgb_afm, _xrgb_calpha,
                        _rot_ok, _odd_ok, _h_free,
                        _vic_fmt, _vic_chroma, _vic_hz);
            } else if (!_2d_dead) {
                /* A job that never retired may still be reading or
                 * writing the arena: only release it when idle. */
                klog("vic: M2 FAILED -- 2D offload stays off\n");
                dma_free(_arena);
                _arena = 0;
            }
        }
    }
    int r = (_2d_state == 1 && !_2d_dead) ? 0 : -1;
    pthread_mutex_unlock(&_vic_lock);
    return r;
}
