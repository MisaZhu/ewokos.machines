/* QCOM8GEN3 ADSP bring-up: SC8280XP ADSP (PAS id 1).
 *
 * One restricted op boots the signed ADSP image: userspace hands the kernel a
 * DMA buffer holding the board firmware (qcadsp8280.mbn), and the kernel does
 * exactly what qcom_mdt_load() + qcom_q6v5_pas do on Linux:
 *
 *   parse ELF32 -> extract metadata (ELF header + program header table)
 *   SCM PAS_INIT(1, metadata) -> PAS_MEM_SETUP(carveout, span)
 *   copy relocated segments into the carveout -> PAS_AUTH_AND_RESET(1)
 *
 * Phase 1 stops at "authenticated and out of reset".  The SMP2P/GLINK/rpmsg
 * transport and the RPMh power-domain vote are intentionally absent and belong
 * to a later phase.
 *
 * Only the fixed ADSP carveout from the board DT is reachable; no caller
 * supplied SMC function, PAS id or destination address is accepted.
 */
#include <stdint.h>
#include <kernel/proc.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/dma.h>
#include <mm/mmu.h>
#include <kprintf.h>
#include <qcom8gen3_adsp_pas.h>
#include <qcom8gen3_adsp_gpr.h>
#include <qcom8gen3_smp2p_wire.h>
#include <qcom8gen3_rpmh_tcs.h>
#include <qcom8gen3_mdt_load.h>
#include "hw_arch.h"
#include "scm-probe.h"

#ifndef QCOM8GEN3_ADSP_DEBUG
#define QCOM8GEN3_ADSP_DEBUG 0
#endif
#define adsp_debug(...) do { if(QCOM8GEN3_ADSP_DEBUG) printf(__VA_ARGS__); } while(0)

#define ADSP_MAX_FW      (16u * 1024u * 1024u)
#define ADSP_META_SIZE   0x4000u
#define ADSP_MDT_TYPE_MASK  0x07000000u
#define ADSP_MDT_TYPE_HASH  0x02000000u

#ifndef QCOM8GEN3_QEMU_TEST
extern void qcom8gen3_scm_smc(uint64_t regs[8]);

static int scm_call(uint32_t fn, uint32_t info, uint64_t a, uint64_t b,
        uint64_t c, int fast, uint64_t *result) {
    uint64_t input[8] = { (fast ? 0xc2000000u : 0x42000000u) | fn,
        info, a, b, c, 0, 0, 0 };
    uint64_t regs[8];
    unsigned n;

    for(n = 0; n < 1024; n++) {
        for(unsigned i = 0; i < 8; i++) regs[i] = input[i];
        __asm__ volatile("dsb sy" ::: "memory");
        qcom8gen3_scm_smc(regs);
        __asm__ volatile("dsb sy" ::: "memory");
        if(regs[0] != 1) break;
        input[0] = 1;
        input[6] = regs[6];
    }
    printf("qcom8gen3 ADSP SCM fn=%x arg=%llx a0=%llx a1=%llx a2=%llx\n", fn,
            (unsigned long long)a,
            (unsigned long long)regs[0], (unsigned long long)regs[1],
            (unsigned long long)regs[2]);
    if((int64_t)regs[0] == -12) return QCOM8GEN3_GPU_PAS_BUSY;
    if(regs[0] == 2) return QCOM8GEN3_GPU_PAS_WAITQ;
    if(regs[0] != 0) return QCOM8GEN3_GPU_PAS_FAILED;
    *result = regs[1];
    return 0;
}
#endif

/* Kernel-only Normal-NC window. VERIFY: must not collide with the NPU staging
 * window (0x5000000000..0x501e00000) or SMEM (0x5020000000). */
#define QCOM8GEN3_ADSP_WIN 0x5010000000ull

static int probed;
static int probe_rc;
static ewokos_addr_t meta_pa;
static void *meta_va;
static int32_t win_pid = -1;
static int booted;

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} adsp_elf32_hdr;

typedef qcom8gen3_mdt_phdr adsp_elf32_phdr;

/* =========================================================================
 * Phase 2/3: RPMh power vote + IPCC + SMEM + SMP2P + GLINK + GPR battmgr.
 *
 * Every board-specific constant below is marked with a VERIFY comment.  These
 * are bring-up guesses transcribed from the cDSP (npu_pas.c) transport and
 * must be confirmed against sc8280xp.dtsi / the production qcadsp8280.mbn
 * before this is trusted on real hardware.
 *
 * QCOM8GEN3_ADSP_TRANSPORT gates the whole Phase 2/3 transport and defaults to 0
 * (off): running it on hardware with the placeholder VERIFY addresses above
 * wedges the board.  Until every constant is confirmed the ADSP boot stays at
 * Phase 1 (SCM PAS auth + reset only) and the battery-manager queries report
 * UNSUPPORTED.  Build with -DQCOM8GEN3_ADSP_TRANSPORT=1 once the constants are real.
 * ========================================================================= */
#ifndef QCOM8GEN3_ADSP_TRANSPORT
#define QCOM8GEN3_ADSP_TRANSPORT 1
#endif

#if !defined(QCOM8GEN3_QEMU_TEST)

/* IPCC: LPASS client (IPCC_CLIENT_LPASS = 3) is the ADSP.  Signal 0 =
 * GLINK/QMP (IPCC_MPROC_SIGNAL_GLINK_QMP), signal 2 = SMP2P
 * (IPCC_MPROC_SIGNAL_SMP2P). */
#define QCOM8GEN3_ADSP_IPCC_BASE      (MMIO_BASE + 0x408000u) /* VERIFY: shared IPCC MMIO window, same as cDSP */
#define QCOM8GEN3_ADSP_IPCC_GLINK     0x00030000u             /* IPCC_CLIENT_LPASS(3), signal 0 (GLINK/QMP) */
#define QCOM8GEN3_ADSP_IPCC_SMP2P     0x00030002u             /* IPCC_CLIENT_LPASS(3), signal 2 (SMP2P) */

/* SMEM: same physical region as cDSP, but a distinct private partition.  The
 * per-host private partition offset is read at runtime from the SMEM partition
 * table (ptable) 4 KiB before the end of the SMEM region (Linux smem.c). */
#define QCOM8GEN3_ADSP_SMEM_PHY       0x80900000ull  /* VERIFY: shared SMEM base */
#define QCOM8GEN3_ADSP_SMEM_SIZE      0x200000ull
#define QCOM8GEN3_ADSP_SMEM_WIN       0x5020000000ull
#define QCOM8GEN3_ADSP_SMEM_PTABLE_MAGIC 0x434f5424U  /* "$TOC" little-endian u32 */
#define QCOM8GEN3_ADSP_SMEM_HOST_ADSP    2U        /* SMEM_HOST_ADSP */
#define QCOM8GEN3_ADSP_SMEM_PRIV_OFF  0x1af000u      /* fallback if ptable unreadable */

/* cmd-db / apps RSC physical windows (shared with cDSP, mapped to ADSP-specific VA windows). */
#define QCOM8GEN3_ADSP_CMDB_PHY       0x80860000ull  /* VERIFY: shared cmd-db */
#define QCOM8GEN3_ADSP_CMDB_SIZE      0x20000u
#define QCOM8GEN3_ADSP_CMDB_WIN       0x5020400000ull
#define QCOM8GEN3_ADSP_RSC_PHY        0x18220000ull  /* VERIFY: apps RSC */
#define QCOM8GEN3_ADSP_RSC_SIZE       0x10000u
#define QCOM8GEN3_ADSP_RSC_WIN        0x5020500000ull
#define QCOM8GEN3_ADSP_TCS_OFF        QCOM8GEN3_RSC_TCS_OFFSET
#define QCOM8GEN3_ADSP_TCS_STRIDE     672u
#define QCOM8GEN3_ADSP_IRQ_ENABLE     QCOM8GEN3_RSC_IRQ_ENABLE
#define QCOM8GEN3_ADSP_IRQ_STATUS     QCOM8GEN3_RSC_IRQ_STATUS
#define QCOM8GEN3_ADSP_IRQ_CLEAR      QCOM8GEN3_RSC_IRQ_CLEAR
#define QCOM8GEN3_ADSP_AMC_ENABLE     (1u << 16)
#define QCOM8GEN3_ADSP_AMC_TRIGGER    (1u << 24)
#define QCOM8GEN3_ADSP_WRITE_RESP_MSGID 0x00010108u

/* SMP2P SMEM item ids: sc8280xp.dtsi "smp2p-adsp" uses qcom,smem = <443>,
 * <429> and qcom,remote-pid = <2>.  IN (443) is where the ADSP writes its
 * slave-kernel entry; OUT (429) is where the AP writes master-kernel. */
#define QCOM8GEN3_ADSP_SMP2P_IN_ITEM  443u
#define QCOM8GEN3_ADSP_SMP2P_OUT_ITEM 429u

/* SMP2P entry/item sizes (mirror NPU). */
#define QCOM8GEN3_ADSP_SMP2P_DATA_SIZE 0x154u
#define QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE 0x158u

/* SMP2P remote state bits (local, internal only). */
#define QCOM8GEN3_ADSP_SMP2P_REMOTE   (1u << 0)
#define QCOM8GEN3_ADSP_SMP2P_READY    (1u << 1)  /* slave-kernel "ready" bit */

/* GLINK channel parameters. */
#define QCOM8GEN3_ADSP_GLINK_VERSION           1u
#define QCOM8GEN3_ADSP_GLINK_FEATURES          1u
#define QCOM8GEN3_ADSP_GLINK_FIFO_RESERVE      16u
#define QCOM8GEN3_ADSP_GLINK_FIFOMAX           16384u
#define QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_SIZE 1024u
#define QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_COUNT 5u
#define QCOM8GEN3_ADSP_GLINK_RX_MAX            4096u

/* Busy/spin delay (copy of npu_delay_ms). */
static void adsp_delay_ms(uint32_t ms) {
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    do {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    } while ((now - start) * 1000ull < freq * ms);
}

static int adsp_map_in_caller_attr(ewokos_addr_t va, ewokos_addr_t pa,
        uint32_t size, uint32_t attr) {
    proc_t *p = proc_get_proc(get_current_proc());
    page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;
    if (map_pages_size(_kernel_info.kernel_vm, va, pa, size, AP_RW_D, attr) != 0)
        return -1;
    if (pvm != 0 && pvm != _kernel_info.kernel_vm &&
            map_pages_size(pvm, va, pa, size, AP_RW_D, attr) != 0)
        return -1;
    flush_tlb();
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int adsp_map_in_caller(ewokos_addr_t va, ewokos_addr_t pa, uint32_t size) {
    return adsp_map_in_caller_attr(va, pa, size, PTE_ATTR_SYS_DMA);
}

/* ---- RPMh command-db / RSC (adapted from npu_pas.c). ---- */
static struct {
    uint32_t control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data,
             cmd_status, cmd_stride;
    int ready;
    uint32_t completions, previous_compl, compl_without_irq;
} adsp_rsc = { 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20, 0, 0, 0, 0 };

static uint8_t adsp_cdb8(uint32_t off) {
    return *(volatile uint8_t *)(QCOM8GEN3_ADSP_CMDB_WIN + off);
}
static uint16_t adsp_cdb16(uint32_t off) {
    return (uint16_t)(adsp_cdb8(off) | ((uint16_t)adsp_cdb8(off + 1) << 8));
}
static uint32_t adsp_cdb32(uint32_t off) {
    return *(volatile uint32_t *)(QCOM8GEN3_ADSP_CMDB_WIN + off);
}

static int adsp_cdb_find(const char *id, uint32_t *addr, uint32_t *aux_off,
        uint32_t *aux_len) {
    uint32_t i;
    if (adsp_cdb32(4) != 0x0c0330dbu) return -1;
    for (i = 0; i < 8; i++) {
        uint32_t rsc = 8 + i * 16;
        uint32_t slave = adsp_cdb16(rsc), hdr, data, count, entries, j;
        if (slave == 0) break;
        hdr = adsp_cdb16(rsc + 2); data = adsp_cdb16(rsc + 4);
        count = adsp_cdb16(rsc + 6);
        entries = 144 + hdr;
        for (j = 0; j < count; j++) {
            uint32_t e = entries + j * 24;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                uint8_t c = (uint8_t)id[k];
                if (adsp_cdb8(e + k) != c) break;
                if (c == 0) { k = 8; break; }
            }
            if (k < 8) continue;
            if (addr) *addr = adsp_cdb32(e + 16);
            if (aux_off) *aux_off = 144 + data + adsp_cdb16(e + 22);
            if (aux_len) *aux_len = adsp_cdb16(e + 20);
            return 0;
        }
    }
    return -1;
}

static uint32_t adsp_rsc_reg(uint32_t tcs, uint32_t reg) {
    return *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
            tcs * QCOM8GEN3_ADSP_TCS_STRIDE + reg);
}
static void adsp_rsc_wr(uint32_t tcs, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
            tcs * QCOM8GEN3_ADSP_TCS_STRIDE + reg) = value;
}
static uint32_t adsp_rsc_cmd(uint32_t tcs, uint32_t cmd, uint32_t reg) {
    return QCOM8GEN3_ADSP_TCS_OFF + tcs * QCOM8GEN3_ADSP_TCS_STRIDE +
            cmd * adsp_rsc.cmd_stride + reg;
}
static int adsp_rsc_wr_sync(uint32_t tcs, uint32_t reg, uint32_t val) {
    adsp_rsc_wr(tcs, reg, val);
    for (int i = 0; i < 100000; i++) {
        if (adsp_rsc_reg(tcs, reg) == val) return 0;
        __asm__ volatile("yield" ::: "memory");
    }
    return -1;
}

static void adsp_rsc_relax(void *context) {
    (void)context;
    for (unsigned i = 0; i < 1000; i++) __asm__ volatile("yield");
}

static uint32_t adsp_rsc_read32(void *context, uint32_t off) {
    (void)context;
    return *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + off);
}

static int adsp_rpmh_write(uint32_t address, uint32_t data) {
    const uint32_t tcs = 0, irq = 1u;
    uint32_t control;
    int i;
    if (!adsp_rsc.ready) return -2;
    control = adsp_rsc_reg(tcs, adsp_rsc.control) & ~QCOM8GEN3_ADSP_AMC_TRIGGER;
    if (adsp_rsc_wr_sync(tcs, adsp_rsc.control, control) != 0) return -3;
    control &= ~QCOM8GEN3_ADSP_AMC_ENABLE;
    if (adsp_rsc_wr_sync(tcs, adsp_rsc.control, control) != 0) return -4;
    if (adsp_rsc_wr_sync(tcs, adsp_rsc.cmd_enable, 0) != 0) return -5;
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
            QCOM8GEN3_ADSP_IRQ_CLEAR) = irq;
    __asm__ volatile("dsb sy" ::: "memory");
    for (i = 0; i < 100000; i++)
        if (!(*(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
                QCOM8GEN3_ADSP_IRQ_STATUS) & irq))
            break;
    if (i == 100000) return -9;
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
            QCOM8GEN3_ADSP_IRQ_ENABLE) =
            *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
                QCOM8GEN3_ADSP_IRQ_ENABLE) | irq;
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + adsp_rsc_cmd(tcs, 0,
                adsp_rsc.cmd_msgid)) = QCOM8GEN3_ADSP_WRITE_RESP_MSGID;
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + adsp_rsc_cmd(tcs, 0,
                adsp_rsc.cmd_addr)) = address;
    *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + adsp_rsc_cmd(tcs, 0,
                adsp_rsc.cmd_data)) = data;
    if (adsp_rsc_wr_sync(tcs, adsp_rsc.cmd_enable, 1) != 0) return -6;
    if (adsp_rsc_wr_sync(tcs, adsp_rsc.control, QCOM8GEN3_ADSP_AMC_ENABLE) != 0)
        return -7;
    if (*(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN +
                adsp_rsc_cmd(tcs, 0, adsp_rsc.cmd_status)) & (1u << 16))
        adsp_rsc.previous_compl++;
    adsp_rsc_wr(tcs, adsp_rsc.control, QCOM8GEN3_ADSP_AMC_ENABLE |
            QCOM8GEN3_ADSP_AMC_TRIGGER);
    const qcom8gen3_rpmh_io_t io = { 0, adsp_rsc_read32, adsp_rsc_relax };
    if (qcom8gen3_rpmh_wait_irq(&io, adsp_rsc_cmd(tcs, 0, adsp_rsc.cmd_status),
                irq, 1000000, &adsp_rsc.compl_without_irq) == 0) {
        adsp_rsc.completions++;
        uint32_t c = adsp_rsc_reg(tcs, adsp_rsc.control) &
                ~QCOM8GEN3_ADSP_AMC_TRIGGER;
        if (adsp_rsc_wr_sync(tcs, adsp_rsc.control, c) != 0 ||
                adsp_rsc_wr_sync(tcs, adsp_rsc.control, 0) != 0 ||
                adsp_rsc_wr_sync(tcs, adsp_rsc.cmd_enable, 0) != 0)
            return -10;
        *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
                QCOM8GEN3_ADSP_IRQ_CLEAR) = irq;
        return 0;
    }
    printf("qcom8gen3 adsp: RPMh fresh IRQ timeout addr=%x data=%x cmd=%x irq=%x\n",
            address, data, *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN +
                adsp_rsc_cmd(tcs, 0, adsp_rsc.cmd_status)),
            *(volatile uint32_t *)(QCOM8GEN3_ADSP_RSC_WIN + QCOM8GEN3_ADSP_TCS_OFF +
                QCOM8GEN3_ADSP_IRQ_STATUS));
    return -8;
}

static int adsp_rpmh_vote(const char *name) {
    uint32_t addr = 0, aux = 0, len = 0, i, count, corner;
    if (adsp_cdb_find(name, &addr, &aux, &len) != 0 || addr == 0) {
        printf("qcom8gen3 adsp: rpmh %s not in cmd-db\n", name);
        return -1;
    }
    count = len / 2;
    for (i = 1; i < count; i++) {
        if (adsp_cdb16(aux + i * 2) == 0) { count = i; break; }
    }
    if (count == 0) count = 1;
    corner = count - 1;
    if (adsp_rpmh_write(addr, corner) != 0) {
        printf("qcom8gen3 adsp: rpmh %s vote failed addr=%x\n", name, addr);
        return -1;
    }
    adsp_debug("qcom8gen3 adsp: rpmh %s addr=%x corner=%u ok\n", name, addr, corner);
    return 0;
}

static int adsp_power_vote(void) {
    uint32_t id;
    if (adsp_map_in_caller(QCOM8GEN3_ADSP_CMDB_WIN, QCOM8GEN3_ADSP_CMDB_PHY,
            QCOM8GEN3_ADSP_CMDB_SIZE) != 0 ||
            adsp_map_in_caller_attr(QCOM8GEN3_ADSP_RSC_WIN, QCOM8GEN3_ADSP_RSC_PHY,
                QCOM8GEN3_ADSP_RSC_SIZE, PTE_ATTR_DEV) != 0) {
        printf("qcom8gen3 adsp: rpmh map failed\n");
        return -1;
    }
    id = *(volatile uint32_t *)QCOM8GEN3_ADSP_RSC_WIN;
    if (((id >> 16) & 0xff) >= 3) {
        adsp_rsc.control = 0x24;
        adsp_rsc.status = 0x28;
        adsp_rsc.cmd_enable = 0x2c;
        adsp_rsc.cmd_msgid = 0x34;
        adsp_rsc.cmd_addr = 0x38;
        adsp_rsc.cmd_data = 0x3c;
        adsp_rsc.cmd_status = 0x40;
        adsp_rsc.cmd_stride = 24;
    }
    adsp_rsc.ready = 1;
    adsp_debug("qcom8gen3 adsp: rpmh rsc id=%x\n", id);
    /* ADSP PAS resources from sc8280xp.dtsi clock-names="xo" and
     * power-domain-names="lcx","lmx": enable the XO clock and the LCX/LMX
     * power domains before PAS auth_and_reset. */
    if (adsp_rpmh_vote("xo.lvl") != 0 || adsp_rpmh_vote("lcx.lvl") != 0 ||
            adsp_rpmh_vote("lmx.lvl") != 0)
        return -1;
    adsp_debug("qcom8gen3 adsp: rpmh power vote done\n");
    return 0;
}

static int adsp_power_release(void) {
    uint32_t addr = 0;
    if (adsp_cdb_find("lmx.lvl", &addr, 0, 0) != 0 || !addr ||
            adsp_rpmh_write(addr, 0) != 0) {
        printf("qcom8gen3 adsp: rpmh lmx.lvl release failed\n");
        return -1;
    }
    return 0;
}

#if QCOM8GEN3_ADSP_TRANSPORT
/* ---- IPCC (IPCC_CLIENT_LPASS = 3). ---- */
static void adsp_ipcc_enable(uint32_t signal) {
    volatile uint32_t *ipcc = (volatile uint32_t *)(MMIO_BASE + 0x408000);
    uint32_t hwirq = (3u << 16) | signal; /* IPCC_CLIENT_LPASS */
    uint32_t config = ipcc[0x08 / 4];
    if (config & 1u) ipcc[0x08 / 4] = config & ~1u;
    ipcc[0x14 / 4] = hwirq;
}

static int adsp_boot_smp2p_notified;

static uint32_t adsp_ipcc_recv(void *data) {
    (void)data;
    return *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x10);
}
static void adsp_ipcc_clear(void *data, uint32_t id) {
    (void)data;
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x1c) = id;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t adsp_ipcc_poll(void) {
    uint32_t signals = 0;
    for (unsigned n = 0; n < 32; n++) {
        uint32_t id = adsp_ipcc_recv(0);
        if (id != QCOM8GEN3_ADSP_IPCC_GLINK && id != QCOM8GEN3_ADSP_IPCC_SMP2P) break;
        adsp_ipcc_clear(0, id);
        signals |= id == QCOM8GEN3_ADSP_IPCC_SMP2P ? 4u : 1u;
    }
    if (signals & 4u) adsp_boot_smp2p_notified = 1;
    return signals;
}

/* ---- SMEM private-item walk (copy of smem_find_private_item). ---- */
static uint8_t *adsp_smem_find_private_item(uint8_t *ph, uint32_t free_off,
        uint16_t want) {
    if (free_off < 32 || free_off > QCOM8GEN3_ADSP_SMEM_SIZE ||
            free_off > *(const uint32_t *)(ph + 8))
        return 0;
    for (uint32_t off = 32; off + 16 <= free_off; off += 4) {
        uint8_t *e = ph + off;
        uint16_t canary = *(uint16_t *)(e + 0);
        uint16_t item = *(uint16_t *)(e + 2);
        uint32_t size = *(uint32_t *)(e + 4);
        uint16_t pad_hdr = *(uint16_t *)(e + 10);
        uint32_t next;
        if (canary != 0xa5a5 || size > 0x10000 || pad_hdr > 0x1000)
            continue;
        next = off + 16 + pad_hdr + ((size + 7u) & ~7u);
        if (next > free_off)
            continue;
        if (item == want)
            return e;
    }
    return 0;
}

/* ---- SMP2P (remote = ADSP(2)). ---- */
static int adsp_smp2p_init_master(uint8_t *data, size_t size) {
    static const char name[] = "master-kernel";
    if (!data || size < 340) return -1;
    for (size_t i = 0; i < size; i++) data[i] = 0;
    data[0] = 0x24; data[1] = 0x53; data[2] = 0x4d; data[3] = 0x50;
    data[5] = 1;  /* SSR_ACK feature */
    data[10] = 2; /* remote pid = ADSP(2) */
    data[12] = 16;
    data[14] = 1;
    for (size_t i = 0; i < sizeof(name); i++) data[20 + i] = (uint8_t)name[i];
    return 0;
}

static uint32_t adsp_smp2p_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int adsp_smp2p_slave_bits(const uint8_t *data, size_t size,
        uint32_t *bits) {
    static const char name[] = "slave-kernel";
    unsigned total, valid;
    if (!data || !bits || size < 20 ||
            adsp_smp2p_le32(data) != 0x504d5324u ||
            (data[4] != 1 && data[4] != 2) ||
            data[8] != 2 || data[9] || data[10] || data[11]) /* remote pid = ADSP(2) */
        return -1;
    total = (unsigned)data[12] | ((unsigned)data[13] << 8);
    valid = (unsigned)data[14] | ((unsigned)data[15] << 8);
    if (!total || total > 16 || valid > total || size < 20 + total * 20)
        return -1;
    for (unsigned i = 0; i < valid; i++) {
        const uint8_t *entry = data + 20 + i * 20;
        unsigned k;
        for (k = 0; k < sizeof(name); k++)
            if (entry[k] != (uint8_t)name[k]) break;
        if (k == sizeof(name)) {
            *bits = adsp_smp2p_le32(entry + 16);
            return 0;
        }
    }
    return -1;
}

static uint32_t adsp_smem_priv_off = QCOM8GEN3_ADSP_SMEM_PRIV_OFF;

/* Resolve the ADSP (SMEM_HOST_ADSP) private-partition offset from the SMEM
 * partition table 4 KiB before the end of the SMEM region.  Requires
 * QCOM8GEN3_ADSP_SMEM_WIN mapped.  Mirrors Linux qcom_smem_map_ptable():
 *   ptable = smem_base + smem_size - 0x1000, magic "$TOC",
 *   entry stride 48 B, offset (bytes) at +0, host0/host1 (u16) at +12/+14. */
static void adsp_smem_resolve_priv_off(void) {
    const uint8_t *s = (const uint8_t *)QCOM8GEN3_ADSP_SMEM_WIN;
    const uint8_t *pt = s + QCOM8GEN3_ADSP_SMEM_SIZE - 0x1000u;
    uint32_t magic = *(const volatile uint32_t *)(pt + 0);
    uint32_t num = *(const volatile uint32_t *)(pt + 8);
    uint32_t found = 0;

    if (magic == QCOM8GEN3_ADSP_SMEM_PTABLE_MAGIC && num <= 32u) {
        for (uint32_t i = 0; i < num; i++) {
            const uint8_t *e = pt + 0x20u + i * 48u;
            uint16_t host0 = *(const volatile uint16_t *)(e + 12);
            uint16_t host1 = *(const volatile uint16_t *)(e + 14);
            if (host0 == QCOM8GEN3_ADSP_SMEM_HOST_ADSP ||
                    host1 == QCOM8GEN3_ADSP_SMEM_HOST_ADSP) {
                found = *(const volatile uint32_t *)(e + 0);
                break;
            }
        }
    }
    if (found)
        adsp_smem_priv_off = found;
    printf("qcom8gen3 adsp: smem priv off=0x%x (magic=0x%x num=%u found=0x%x)\n",
            adsp_smem_priv_off, magic, num, found);
}

static void adsp_smp2p_prepare(void) {
    uint8_t *s = (uint8_t *)QCOM8GEN3_ADSP_SMEM_WIN;
    uint8_t *ph;
    uint32_t fU, fC;
    uint8_t *w;

    if (adsp_map_in_caller(QCOM8GEN3_ADSP_SMEM_WIN, QCOM8GEN3_ADSP_SMEM_PHY,
            QCOM8GEN3_ADSP_SMEM_SIZE) != 0) {
        printf("qcom8gen3 adsp: smem map failed\n");
        return;
    }
    adsp_smem_resolve_priv_off();
    ph = s + adsp_smem_priv_off;
    fU = *(uint32_t *)(ph + 12);
    fC = *(uint32_t *)(ph + 16);

    /* Patch an existing AP-owned outbound item (429). */
    for (w = ph + 32; w + 16 <= ph + fU; ) {
        uint16_t canary = *(uint16_t *)(w + 0);
        uint16_t item = *(uint16_t *)(w + 2);
        uint32_t isz = *(uint32_t *)(w + 4);
        if (canary != 0xa5a5 || isz > 0x10000) break;
        if (item == QCOM8GEN3_ADSP_SMP2P_OUT_ITEM) {
            uint32_t old_size = isz;
            uint32_t next = (uint32_t)(w - ph) + 16 + ((old_size + 7u) & ~7u);
            if (old_size < QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE) {
                uint32_t grow = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE - old_size;
                if (*(uint32_t *)(ph + 12) + grow > fC) {
                    printf("qcom8gen3 adsp: SMP2P item 429 cannot grow old=%x\n", old_size);
                    break;
                }
                for (uint32_t j = *(uint32_t *)(ph + 12); j > next; j--)
                    ph[j + grow - 1] = ph[j - 1];
                *(uint32_t *)(ph + 12) += grow;
                isz = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE;
            }
            *(uint32_t *)(w + 4) = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE;
            *(uint16_t *)(w + 8) = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE - QCOM8GEN3_ADSP_SMP2P_DATA_SIZE;
            *(uint16_t *)(w + 10) = 0;
            *(uint32_t *)(w + 12) = 0;
            adsp_smp2p_init_master((uint8_t *)(w + 16), QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE);
            __asm__ volatile("dsb sy" ::: "memory");
            ((uint8_t *)(w + 16))[4] = 2; /* publish version last */
            __asm__ volatile("dsb sy" ::: "memory");
            adsp_debug("qcom8gen3 adsp: patched SMP2P item 429 master-kernel\n");
            return;
        }
        w += 16 + *(uint16_t *)(w + 10) + ((isz + 7u) & ~7u);
    }

    /* Create the outbound item if space remains. */
    if (fU + 16 + QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE <= fC) {
        uint8_t *e = ph + fU;
        *(uint16_t *)(e + 0) = 0xa5a5;
        *(uint16_t *)(e + 2) = QCOM8GEN3_ADSP_SMP2P_OUT_ITEM;
        *(uint32_t *)(e + 4) = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE;
        *(uint16_t *)(e + 8) = QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE - QCOM8GEN3_ADSP_SMP2P_DATA_SIZE;
        *(uint16_t *)(e + 10) = 0;
        *(uint32_t *)(e + 12) = 0;
        adsp_smp2p_init_master(e + 16, QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE);
        __asm__ volatile("dsb sy" ::: "memory");
        (e + 16)[4] = 2;
        __asm__ volatile("dsb sy" ::: "memory");
        *(uint32_t *)(ph + 12) = fU + 16 + QCOM8GEN3_ADSP_SMP2P_ITEM_SIZE;
        adsp_debug("qcom8gen3 adsp: created SMP2P item 429\n");
    }
}

static int adsp_smp2p_read_remote(uint32_t values[4]) {
    uint8_t *s = (uint8_t *)QCOM8GEN3_ADSP_SMEM_WIN;
    uint8_t *ph = s + adsp_smem_priv_off;
    uint32_t fU = *(volatile uint32_t *)(ph + 12);
    const uint8_t *it;
    const uint8_t *d;
    uint32_t size, bits;
    uint16_t padding, padding_hdr;

    if (!booted) return -1;
    it = adsp_smem_find_private_item(ph, fU, QCOM8GEN3_ADSP_SMP2P_IN_ITEM);
    if (!it) return -1;
    size = *(const uint32_t *)(it + 4);
    padding = *(const uint16_t *)(it + 8);
    padding_hdr = *(const uint16_t *)(it + 10);
    if (padding > size) return -1;
    d = it + 16 + padding_hdr;
    if (size - padding < 20 || *(const uint32_t *)d != 0x504d5324u ||
            (d[4] != 1 && d[4] != 2))
        return -1;
    /* Match qcom_smp2p_negotiate(): production ADSP advertises v1, while
     * AP initially offers v2. Downgrade and kick before waiting for entries. */
    const uint8_t *out_item = adsp_smem_find_private_item(ph, fU,
            QCOM8GEN3_ADSP_SMP2P_OUT_ITEM);
    if (out_item) {
        uint8_t *out = (uint8_t *)out_item + 16 +
                *(const uint16_t *)(out_item + 10);
        int notify = 0;
        if (out[4] > d[4]) {
            out[4] = d[4];
            printf("qcom8gen3 adsp: SMP2P negotiated version %u\n", d[4]);
            notify = 1;
        }
        if (out[4] == d[4]) {
            out[5] &= d[5]; out[6] &= d[6]; out[7] &= d[7];
            if ((out[5] & 1u) &&
                    ((d[16] & 1u) != ((out[16] >> 1) & 1u))) {
                out[16] ^= 2u; /* mirror remote RESTART_DONE as RESTART_ACK */
                notify = 1;
            }
        }
        if (notify) {
            __asm__ volatile("dsb sy" ::: "memory");
            *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) =
                    QCOM8GEN3_ADSP_IPCC_SMP2P;
            __asm__ volatile("dsb sy" ::: "memory");
        }
    }
    if (adsp_smp2p_slave_bits(d, size - padding, &bits) != 0)
        return -1;
    for (unsigned i = 0; i < 4; i++)
        values[i] = (bits >> i) & 1u;
    return 0;
}

static uint32_t adsp_smp2p_refresh_state(void) {
    uint32_t state = 0;
    uint32_t v[4];
    (void)adsp_ipcc_poll();
    if (adsp_smp2p_read_remote(v) == 0) {
        state |= QCOM8GEN3_ADSP_SMP2P_REMOTE;
        if (v[1]) state |= QCOM8GEN3_ADSP_SMP2P_READY;
    }
    return state;
}

static int adsp_wait_for_ready(void) {
    for (unsigned attempt = 0; attempt < 50; attempt++) {
        uint32_t state = adsp_smp2p_refresh_state();
        if ((state & QCOM8GEN3_ADSP_SMP2P_READY) && adsp_boot_smp2p_notified)
            return 0;
        *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = QCOM8GEN3_ADSP_IPCC_SMP2P;
        __asm__ volatile("dsb sy" ::: "memory");
        adsp_delay_ms(100);
    }
    printf("qcom8gen3 adsp: ADSP ready not observed notify=%d pending=%x\n",
            adsp_boot_smp2p_notified, adsp_ipcc_recv(0));
    return QCOM8GEN3_ADSP_PAS_FAILED;
}

/* ---- GLINK transport (single GPR channel, no FastRPC/QRTR state). ---- */
typedef struct {
    uint8_t *tx;
    uint8_t *rx;
    volatile uint32_t *desc;
    uint32_t len;
    uint16_t cid;
    uint16_t remote_cid;
    uint32_t remote_iid;
    uint32_t remote_isize;
    int remote_valid;
    uint32_t local_iid;
    uint32_t local_iid_count;
    int version_sent;
    int version_ack;
    int open_sent;
    int open_ack;
    int ready;
    uint32_t rx_len;
    int rx_pending;
    uint8_t rx_buf[QCOM8GEN3_ADSP_GLINK_RX_MAX];
} adsp_glink_state_t;

static adsp_glink_state_t adsp_glink;

static uint32_t adsp_glink_ring_add(uint32_t p, uint32_t n, uint32_t len) {
    p += n;
    while (p >= len) p -= len;
    return p;
}
static uint32_t adsp_glink_ring_used(uint32_t head, uint32_t tail, uint32_t len) {
    return head >= tail ? head - tail : len - tail + head;
}
static void adsp_glink_ring_copy(uint8_t *ring, uint32_t pos, void *dst, uint32_t len) {
    uint32_t first = len;
    if (pos + first > adsp_glink.len) first = adsp_glink.len - pos;
    memcpy(dst, ring + pos, first);
    if (first != len) memcpy((uint8_t *)dst + first, ring, len - first);
}
static void adsp_glink_ring_write(uint8_t *ring, uint32_t pos, const void *src, uint32_t len) {
    uint32_t first = len;
    if (pos + first > adsp_glink.len) first = adsp_glink.len - pos;
    memcpy(ring + pos, src, first);
    if (first != len) memcpy(ring, (const uint8_t *)src + first, len - first);
}
static void adsp_glink_rx_advance(uint32_t tail) {
    __asm__ volatile("dmb sy" ::: "memory");
    adsp_glink.desc[2] = tail;
}

static int adsp_glink_tx_frame(uint16_t cmd, uint16_t p1, uint32_t p2,
        const void *data, uint32_t data_len) {
    uint8_t hdr[8];
    uint32_t head, tail, total;
    if (!adsp_glink.ready || data_len > QCOM8GEN3_ADSP_GLINK_FIFOMAX)
        return -1;
    total = (8u + data_len + 7u) & ~7u;
    head = adsp_glink.desc[1];
    tail = adsp_glink.desc[0];
    __asm__ volatile("dmb sy" ::: "memory");
    if (adsp_glink.len - adsp_glink_ring_used(head, tail, adsp_glink.len) <
            total + QCOM8GEN3_ADSP_GLINK_FIFO_RESERVE)
        return -2;
    hdr[0] = (uint8_t)cmd; hdr[1] = (uint8_t)(cmd >> 8);
    hdr[2] = (uint8_t)p1; hdr[3] = (uint8_t)(p1 >> 8);
    hdr[4] = (uint8_t)p2; hdr[5] = (uint8_t)(p2 >> 8);
    hdr[6] = (uint8_t)(p2 >> 16); hdr[7] = (uint8_t)(p2 >> 24);
    adsp_glink_ring_write(adsp_glink.tx, head, hdr, sizeof(hdr));
    head = adsp_glink_ring_add(head, sizeof(hdr), adsp_glink.len);
    if (data_len) {
        adsp_glink_ring_write(adsp_glink.tx, head, data, data_len);
        head = adsp_glink_ring_add(head, data_len, adsp_glink.len);
    }
    while (head & 7u) {
        adsp_glink.tx[head] = 0;
        head = adsp_glink_ring_add(head, 1, adsp_glink.len);
    }
    __asm__ volatile("dmb sy" ::: "memory");
    adsp_glink.desc[1] = head;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = QCOM8GEN3_ADSP_IPCC_GLINK;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int adsp_glink_tx_data_frame(uint16_t cmd, uint16_t cid, uint32_t iid,
        const void *data, uint32_t size, uint32_t left) {
    uint8_t hdr[16];
    uint32_t head, tail, total;
    if (!adsp_glink.ready || size > QCOM8GEN3_ADSP_GLINK_FIFOMAX)
        return -1;
    total = (16u + size + 7u) & ~7u;
    head = adsp_glink.desc[1];
    tail = adsp_glink.desc[0];
    __asm__ volatile("dmb sy" ::: "memory");
    if (adsp_glink.len - adsp_glink_ring_used(head, tail, adsp_glink.len) <
            total + QCOM8GEN3_ADSP_GLINK_FIFO_RESERVE)
        return -2;
    hdr[0] = (uint8_t)cmd; hdr[1] = (uint8_t)(cmd >> 8);
    hdr[2] = (uint8_t)cid; hdr[3] = (uint8_t)(cid >> 8);
    hdr[4] = (uint8_t)iid; hdr[5] = (uint8_t)(iid >> 8);
    hdr[6] = (uint8_t)(iid >> 16); hdr[7] = (uint8_t)(iid >> 24);
    hdr[8] = (uint8_t)size; hdr[9] = (uint8_t)(size >> 8);
    hdr[10] = (uint8_t)(size >> 16); hdr[11] = (uint8_t)(size >> 24);
    hdr[12] = (uint8_t)left; hdr[13] = (uint8_t)(left >> 8);
    hdr[14] = (uint8_t)(left >> 16); hdr[15] = (uint8_t)(left >> 24);
    adsp_glink_ring_write(adsp_glink.tx, head, hdr, sizeof(hdr));
    head = adsp_glink_ring_add(head, sizeof(hdr), adsp_glink.len);
    adsp_glink_ring_write(adsp_glink.tx, head, data, size);
    head = adsp_glink_ring_add(head, size, adsp_glink.len);
    while (head & 7u) {
        adsp_glink.tx[head] = 0;
        head = adsp_glink_ring_add(head, 1, adsp_glink.len);
    }
    __asm__ volatile("dmb sy" ::: "memory");
    adsp_glink.desc[1] = head;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = QCOM8GEN3_ADSP_IPCC_GLINK;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static void adsp_glink_advertise_intent(void) {
    uint8_t data[8];
    uint32_t size = QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_SIZE;
    data[0] = (uint8_t)size; data[1] = (uint8_t)(size >> 8);
    data[2] = (uint8_t)(size >> 16); data[3] = (uint8_t)(size >> 24);
    for (uint32_t iid = 1; iid <= QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_COUNT; iid++) {
        data[4] = (uint8_t)iid; data[5] = (uint8_t)(iid >> 8);
        data[6] = (uint8_t)(iid >> 16); data[7] = (uint8_t)(iid >> 24);
        (void)adsp_glink_tx_frame(5, adsp_glink.cid, 1, data, sizeof(data));
    }
    adsp_glink.local_iid_count = QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_COUNT;
}

static void adsp_glink_send_local_open(void) {
    static const char name[] = QCOM8GEN3_PMIC_GLINK_CHANNEL;
    adsp_glink.cid = 1;
    adsp_glink.open_sent = 1;
    (void)adsp_glink_tx_frame(2, adsp_glink.cid, sizeof(name), name, sizeof(name));
}

static void adsp_glink_request_remote_intent(uint32_t size) {
    adsp_glink.remote_valid = 0;
    adsp_glink.remote_isize = 0;
    adsp_glink.remote_iid = 0;
    (void)adsp_glink_tx_frame(7, adsp_glink.cid, size, 0, 0);
}

static int adsp_glink_handle_rx_data(uint32_t tail, uint32_t avail,
        uint16_t cid, uint32_t iid) {
    uint8_t h[8];
    uint32_t chunk, left, frame;
    if (avail < 16) return 0;
    adsp_glink_ring_copy(adsp_glink.rx, adsp_glink_ring_add(tail, 8, adsp_glink.len),
            h, sizeof(h));
    chunk = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
            ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    left = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
            ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    frame = 16u + chunk;
    if (chunk > QCOM8GEN3_ADSP_GLINK_RX_MAX - adsp_glink.rx_len || avail < frame)
        return -1;
    adsp_glink_ring_copy(adsp_glink.rx, adsp_glink_ring_add(tail, 16, adsp_glink.len),
            adsp_glink.rx_buf + adsp_glink.rx_len, chunk);
    adsp_glink.rx_len += chunk;
    adsp_glink_rx_advance(adsp_glink_ring_add(tail, (frame + 7u) & ~7u, adsp_glink.len));
    if (left == 0) {
        if (cid == adsp_glink.remote_cid)
            adsp_glink.rx_pending = 1;
        if (cid == adsp_glink.remote_cid && iid >= 1 &&
                iid <= adsp_glink.local_iid_count)
            (void)adsp_glink_tx_frame(14, adsp_glink.cid, iid, 0, 0);
    }
    return 1;
}

static int adsp_glink_poll(void) {
    int handled = 0;
    if (!adsp_glink.ready) return 0;
    (void)adsp_ipcc_poll();
    for (unsigned pass = 0; pass < 64; pass++) {
        uint32_t head = adsp_glink.desc[3], tail = adsp_glink.desc[2];
        __asm__ volatile("dmb sy" ::: "memory");
        uint32_t avail = adsp_glink_ring_used(head, tail, adsp_glink.len);
        uint8_t h[8];
        uint16_t cmd, p1;
        uint32_t p2, frame = 8;
        if (avail < sizeof(h)) break;
        adsp_glink_ring_copy(adsp_glink.rx, tail, h, sizeof(h));
        cmd = (uint16_t)h[0] | ((uint16_t)h[1] << 8);
        p1 = (uint16_t)h[2] | ((uint16_t)h[3] << 8);
        p2 = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
             ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        handled++;
        if (cmd == 9 || cmd == 12) {
            if (adsp_glink_handle_rx_data(tail, avail, p1, p2) < 0) break;
            continue;
        }
        if (cmd == 0 || cmd == 1) {
            if (cmd == 0)
                (void)adsp_glink_tx_frame(1, QCOM8GEN3_ADSP_GLINK_VERSION,
                        QCOM8GEN3_ADSP_GLINK_FEATURES, 0, 0);
            adsp_glink.version_ack = 1;
            if (adsp_glink.open_ack && !adsp_glink.remote_valid)
                adsp_glink_request_remote_intent(QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_SIZE);
        } else if (cmd == 2) {
            uint32_t name_len = p2 & 0xffffu;
            char name[64];
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            if (avail < 8u + name_len) break;
            adsp_glink_ring_copy(adsp_glink.rx, adsp_glink_ring_add(tail, 8, adsp_glink.len),
                    name, name_len);
            name[name_len] = 0;
            (void)adsp_glink_tx_frame(4, p1, 0, 0, 0);
            frame += (name_len + 7u) & ~7u;
            if (strcmp(name, QCOM8GEN3_PMIC_GLINK_CHANNEL) == 0) {
                adsp_glink.remote_cid = p1;
                adsp_glink.local_iid = 1;
                adsp_glink.remote_iid = 0;
                adsp_glink.open_ack = 0;
                adsp_glink_send_local_open();
            }
        } else if (cmd == 4) {
            if (adsp_glink.open_sent && p1 == adsp_glink.cid) {
                adsp_glink.open_ack = 1;
                adsp_glink_advertise_intent();
                if (adsp_glink.version_ack && !adsp_glink.remote_valid)
                    adsp_glink_request_remote_intent(QCOM8GEN3_ADSP_GLINK_LOCAL_INTENT_SIZE);
            }
        } else if (cmd == 5) {
            uint32_t count = p2;
            uint32_t best_size = 0, best_iid = 0;
            if (count > 32) break;
            frame += count * 8u;
            if (avail < frame) break;
            if (p1 == adsp_glink.remote_cid && count) {
                uint8_t d[8];
                for (uint32_t k = 0; k < count; k++) {
                    adsp_glink_ring_copy(adsp_glink.rx, adsp_glink_ring_add(tail,
                            8u + k * 8u, adsp_glink.len), d, sizeof(d));
                    uint32_t isize = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                        ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
                    uint32_t iid = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
                        ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
                    if (isize > best_size) { best_size = isize; best_iid = iid; }
                }
                if (best_size) {
                    adsp_glink.remote_isize = best_size;
                    adsp_glink.remote_iid = best_iid;
                    adsp_glink.remote_valid = 1;
                }
            }
        } else if (cmd == 8) {
            /* RX_INTENT_ACK: nothing to track. */
        } else if (cmd == 6 || cmd == 14) {
            if (p1 == adsp_glink.remote_cid && p2 == adsp_glink.remote_iid) {
                adsp_glink.remote_valid = (cmd == 14);
                if (!adsp_glink.remote_valid) {
                    adsp_glink.remote_iid = 0;
                    adsp_glink.remote_isize = 0;
                }
            }
        } else if (cmd == 7) {
            if (p1 == adsp_glink.remote_cid && adsp_glink.local_iid) {
                (void)adsp_glink_tx_frame(8, adsp_glink.cid, 1, 0, 0);
                adsp_glink_advertise_intent();
            }
        } else if (cmd == 3) {
            (void)adsp_glink_tx_frame(11, p1, 0, 0, 0);
        } else {
            adsp_debug("qcom8gen3 adsp: glink rx unhandled cmd=%u p1=%u p2=%u\n",
                    cmd, p1, p2);
        }
        adsp_glink_rx_advance(adsp_glink_ring_add(tail, (frame + 7u) & ~7u, adsp_glink.len));
    }
    return handled;
}

static int adsp_glink_init(void) {
    uint8_t *s;
    uint8_t *ph;
    uint32_t fU;
    const uint8_t *i478, *i479, *i480;

    if (adsp_glink.ready) return 0;
    s = (uint8_t *)QCOM8GEN3_ADSP_SMEM_WIN;
    ph = s + adsp_smem_priv_off;
    i478 = i479 = i480 = 0;
    for (unsigned try = 0; try < 50; try++) {
        fU = *(volatile uint32_t *)(ph + 12);
        i478 = adsp_smem_find_private_item(ph, fU, 478);
        i479 = adsp_smem_find_private_item(ph, fU, 479);
        i480 = adsp_smem_find_private_item(ph, fU, 480);
        if (i478 && i479 && i480) break;
        *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = QCOM8GEN3_ADSP_IPCC_SMP2P;
        __asm__ volatile("dsb sy" ::: "memory");
        adsp_delay_ms(100);
    }
    if (!i478 || !i479 || !i480) {
        adsp_debug("qcom8gen3 adsp: glink items %d %d %d\n", i478 != 0, i479 != 0, i480 != 0);
        return -1;
    }
    memset(&adsp_glink, 0, sizeof(adsp_glink));
    adsp_glink.desc = (volatile uint32_t *)(i478 + 16);
    adsp_glink.tx = (uint8_t *)(i479 + 16);
    adsp_glink.rx = (uint8_t *)(i480 + 16);
    adsp_glink.len = QCOM8GEN3_ADSP_GLINK_FIFOMAX;
    adsp_glink.ready = 1;
    adsp_glink.desc[1] = 0;
    adsp_glink.desc[2] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    (void)adsp_glink_tx_frame(0, QCOM8GEN3_ADSP_GLINK_VERSION, QCOM8GEN3_ADSP_GLINK_FEATURES, 0, 0);
    adsp_glink.version_sent = 1;
    return 0;
}

static int adsp_glink_send_data(const void *data, uint32_t size) {
    uint32_t off = 0, chunk;
    if (!adsp_glink.cid || !size || size > QCOM8GEN3_ADSP_GLINK_FIFOMAX)
        return -1;
    if (!adsp_glink.remote_valid || adsp_glink.remote_isize < size) {
        adsp_glink_request_remote_intent(size);
        for (unsigned tries = 0; tries < 5000; tries++) {
            (void)adsp_glink_poll();
            if (adsp_glink.remote_valid && adsp_glink.remote_isize >= size)
                break;
            adsp_delay_ms(1);
        }
        if (!adsp_glink.remote_valid || adsp_glink.remote_isize < size)
            return -2;
    }
    while (off < size) {
        chunk = size - off;
        if (chunk > 8192) chunk = 8192;
        if (adsp_glink_tx_data_frame(off ? 12 : 9, adsp_glink.cid,
                adsp_glink.remote_iid, (const uint8_t *)data + off, chunk,
                size - off - chunk) != 0)
            return -1;
        off += chunk;
    }
    adsp_glink.remote_valid = 0;
    return 0;
}

static int adsp_glink_recv(uint8_t **buf, uint32_t *len) {
    if (!adsp_glink.rx_pending)
        return 0;
    if (buf) *buf = adsp_glink.rx_buf;
    if (len) *len = adsp_glink.rx_len;
    adsp_glink.rx_pending = 0;
    adsp_glink.rx_len = 0;
    return 1;
}

/* ---- PMIC GLINK battmgr (bare PMIC GLINK messages over glink). ---- */
/* Send a request and poll for the response whose opcode matches.  The glink
 * transport carries the bare PMIC GLINK message with no rpmsg_hdr; see
 * qcom8gen3_adsp_gpr.h.  Returns 0 with the resp pointer and length set to the
 * reassembled payload (the static glink rx buffer, valid until the next
 * recv), -1 on timeout / transport failure. */
static int adsp_battmgr_transact(const uint8_t *req, uint32_t req_len,
        uint32_t resp_opcode, uint8_t **resp, uint32_t *resp_len) {
    uint8_t *rx;
    uint32_t rx_len;
    int attempts;

    if (adsp_glink_send_data(req, req_len) != 0)
        return -1;
    for (attempts = 0; attempts < 500; attempts++) {
        (void)adsp_glink_poll();
        if (adsp_glink_recv(&rx, &rx_len)) {
            if (rx_len >= 12 &&
                    qcom8gen3_battmgr_get_le32(rx + 0) == QCOM8GEN3_BATTMGR_OWNER &&
                    qcom8gen3_battmgr_get_le32(rx + 8) == resp_opcode) {
                *resp = rx;
                *resp_len = rx_len;
                return 0;
            }
        }
        adsp_delay_ms(1);
    }
    return -1;
}

/* last_full_capacity (mWh) from BAT_INFO; essentially static, queried once. */
static uint32_t adsp_battmgr_last_full;

/* Read the real state of charge from the ADSP battery manager, mirroring
 * qcom_battmgr_sc8280xp_callback(): BAT_INFO gives last_full_capacity, then
 * BAT_STATUS gives the live capacity, and percent = 100 * capacity / full.
 * Returns 0 on success, -1 on transport failure. */
static int adsp_battmgr_query_battery(uint32_t *percent, int *charging,
        int *present) {
    uint8_t req[QCOM8GEN3_BATTMGR_UPDATE_REQ_SIZE];
    uint8_t *rx;
    uint32_t rx_len, cap_mwh, state, source, design;
    size_t req_len;

    if (!booted || !adsp_glink.ready || !adsp_glink.open_ack)
        return -1;

    if (adsp_battmgr_last_full == 0) {
        req_len = qcom8gen3_battmgr_build_update_req(req, QCOM8GEN3_BATTMGR_BAT_INFO);
        if (adsp_battmgr_transact(req, (uint32_t)req_len,
                    QCOM8GEN3_BATTMGR_BAT_INFO, &rx, &rx_len) != 0)
            return -1;
        design = 0;
        if (qcom8gen3_battmgr_parse_info(rx, rx_len, &adsp_battmgr_last_full,
                    &design) != 0)
            return -1;
        if (adsp_battmgr_last_full == 0)
            adsp_battmgr_last_full = design;
        if (adsp_battmgr_last_full == 0)
            return -1;
    }

    req_len = qcom8gen3_battmgr_build_update_req(req, QCOM8GEN3_BATTMGR_BAT_STATUS);
    if (adsp_battmgr_transact(req, (uint32_t)req_len,
                QCOM8GEN3_BATTMGR_BAT_STATUS, &rx, &rx_len) != 0)
        return -1;
    cap_mwh = state = source = 0;
    if (qcom8gen3_battmgr_parse_status(rx, rx_len, &cap_mwh, &state, &source,
                NULL) != 0)
        return -1;

    *percent = (uint32_t)((100ull * cap_mwh) / adsp_battmgr_last_full);
    if (*percent > 100)
        *percent = 100;
    /* qcom_battmgr_sc8280xp_callback() tests the discharging bit first:
     * it wins over the charging bit when the firmware sets both. */
    *charging = !(state & QCOM8GEN3_BATT_STATE_DISCHARGING) &&
            (state & QCOM8GEN3_BATT_STATE_CHARGING) != 0;
    *present = 1;
    return 0;
}

#endif /* QCOM8GEN3_ADSP_TRANSPORT */
#endif /* !QCOM8GEN3_QEMU_TEST */

#ifdef QCOM8GEN3_QEMU_TEST
static int adsp_probe(void) { return QCOM8GEN3_ADSP_PAS_UNSUPPORTED; }
#else
static int adsp_probe(void) {
    int ret = qcom8gen3_scm_probe(scm_call);

    adsp_debug("qcom8gen3 adsp: scm probe ret=%d\n", ret);
    if(ret) return ret;
    return 0;
}

static int owned_dma(ewokos_addr_t pa, ewokos_addr_t len, uint32_t max,
        uint32_t *capacity) {
    proc_t *p = proc_get_proc(get_current_proc());
    uint32_t cap = dma_size(p->info.pid, pa);

    if(capacity) *capacity = cap;
    return len && len <= max && !(pa & 0xfff) && cap >= len;
}

static uint64_t stage_and_boot(void *fw, uint32_t fw_size, uint64_t fw_pa,
        uint32_t capacity) {
    const adsp_elf32_hdr *eh = (const adsp_elf32_hdr *)fw;
    const adsp_elf32_phdr *ph;
    uint64_t min_addr = ~0ull, max_addr = 0, meta_len;
    uint32_t i, hash = 0, hdr_len;
    int ret;
    uint64_t result = 0;
    uint8_t *dst;

    if(fw_size < sizeof(*eh) || eh->e_ident[0] != 0x7f ||
            eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
            eh->e_ident[3] != 'F' || eh->e_ident[4] != 1)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    if(eh->e_phnum < 2 || eh->e_phnum > 32 ||
            eh->e_phentsize != sizeof(*ph) ||
            (uint64_t)eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(*ph) > fw_size)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    ph = (const adsp_elf32_phdr *)((const uint8_t *)fw + eh->e_phoff);

    for(i = 0; i < eh->e_phnum; i++) {
        uint64_t end;
        if(!qcom8gen3_mdt_loadable(&ph[i])) {
            if(!hash && (ph[i].p_flags & ADSP_MDT_TYPE_MASK) ==
                    ADSP_MDT_TYPE_HASH)
                hash = i;
            continue;
        }
        end = (uint64_t)ph[i].p_paddr + ph[i].p_memsz;
        if(ph[i].p_paddr < min_addr) min_addr = ph[i].p_paddr;
        if(end > max_addr) max_addr = end;
    }
    if(!hash || min_addr == ~0ull || max_addr <= min_addr)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    max_addr = (max_addr + 0xfff) & ~0xfffull;
    /* The firmware only needs to *fit* in the carveout; its loadable span is
     * the actual image size, not the 32 MiB reservation. */
    if(max_addr - min_addr > QCOM8GEN3_ADSP_SPAN)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    if(qcom8gen3_mdt_validate_loads(fw_size, ph, eh->e_phnum, min_addr,
                QCOM8GEN3_ADSP_SPAN) != 0)
        return QCOM8GEN3_ADSP_PAS_INVALID;

    /* metadata = ELF header + program headers + hash segment, the exact
     * qcom_mdt_read_metadata() layout Linux passes to PAS_INIT_IMAGE. */
    hdr_len = eh->e_phoff + (uint32_t)eh->e_phnum * (uint32_t)sizeof(*ph);
    meta_len = (uint64_t)hdr_len + ph[hash].p_filesz;
    if(meta_len > ADSP_META_SIZE || hdr_len > fw_size ||
            (uint64_t)ph[hash].p_offset + ph[hash].p_filesz > fw_size)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    /* The metadata lives in the caller's DMA buffer, after the image, so it
     * is mapped in the syscall's address space (kernel-private dma_alloc
     * pages are not). */
    {
        uint32_t off = (fw_size + 0xfffu) & ~0xfffu;
        if((uint64_t)off + meta_len > capacity)
            return QCOM8GEN3_ADSP_PAS_INVALID;
        meta_va = (uint8_t *)fw + off;
        meta_pa = fw_pa + off;
    }
    for(i = 0; i < hdr_len; i++)
        ((uint8_t *)meta_va)[i] = ((const uint8_t *)fw)[i];
    for(i = 0; i < ph[hash].p_filesz; i++)
        ((uint8_t *)meta_va)[hdr_len + i] =
                ((const uint8_t *)fw)[ph[hash].p_offset + i];
    __asm__ volatile("dsb sy" ::: "memory");

    /* Power-domain vote (LCX/LMX) is a Phase-1 prerequisite: Linux
     * qcom_q6v5_prepare() enables the ADSP proxy domains before
     * PAS_AUTH_AND_RESET; without it the SCM call hangs and wedges the board. */
    if(adsp_power_vote() != 0)
        return QCOM8GEN3_ADSP_PAS_POWER;

    /* Phase 2/3 transport (IPCC/SMP2P/GLINK) is still gated. */
#if QCOM8GEN3_ADSP_TRANSPORT
    adsp_ipcc_enable(0); /* GLINK */
    adsp_ipcc_enable(2); /* SMP2P */
    adsp_smp2p_prepare();
    /* Kick the SMP2P signal so the ADSP sees the freshly written master item. */
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = QCOM8GEN3_ADSP_IPCC_SMP2P;
    __asm__ volatile("dsb sy" ::: "memory");
#endif

    printf("qcom8gen3 adsp: fw min=%llx max=%llx span=%llx meta=%llu hash=%u\n",
            (unsigned long long)min_addr, (unsigned long long)max_addr,
            (unsigned long long)(max_addr - min_addr),
            (unsigned long long)meta_len, hash);

    ret = scm_call(0x201, 0x82, QCOM8GEN3_ADSP_PAS_ID, meta_pa,
            (uint32_t)meta_len, 0, &result);
    adsp_debug("qcom8gen3 adsp: init ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == QCOM8GEN3_GPU_PAS_BUSY ? QCOM8GEN3_ADSP_PAS_BUSY :
            QCOM8GEN3_ADSP_PAS_INIT;
    if(result) return QCOM8GEN3_ADSP_PAS_INIT;

    ret = scm_call(0x202, 3, QCOM8GEN3_ADSP_PAS_ID, QCOM8GEN3_ADSP_CARVEOUT,
            QCOM8GEN3_ADSP_SPAN, 0, &result);
    adsp_debug("qcom8gen3 adsp: mem_setup ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == QCOM8GEN3_GPU_PAS_BUSY ? QCOM8GEN3_ADSP_PAS_BUSY :
            QCOM8GEN3_ADSP_PAS_MEM;
    if(result) return QCOM8GEN3_ADSP_PAS_MEM;

    {
        /* A syscall runs on the caller's page tables, so the carveout must be
         * present in both the kernel and the current process address space. */
        proc_t *p = proc_get_proc(get_current_proc());
        page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;

        if(p->info.pid != win_pid) {
            if(map_pages_size(_kernel_info.kernel_vm, QCOM8GEN3_ADSP_WIN,
                        QCOM8GEN3_ADSP_CARVEOUT, QCOM8GEN3_ADSP_SPAN,
                        AP_RW_D, PTE_ATTR_SYS_DMA) != 0)
                return QCOM8GEN3_ADSP_PAS_FAILED;
            if(pvm != 0 && pvm != _kernel_info.kernel_vm &&
                    map_pages_size(pvm, QCOM8GEN3_ADSP_WIN,
                        QCOM8GEN3_ADSP_CARVEOUT, QCOM8GEN3_ADSP_SPAN,
                        AP_RW_D, PTE_ATTR_SYS_DMA) != 0)
                return QCOM8GEN3_ADSP_PAS_FAILED;
            win_pid = p->info.pid;
        }
        flush_tlb();
        __asm__ volatile("dsb sy" ::: "memory");
    }
    dst = (uint8_t *)QCOM8GEN3_ADSP_WIN;
    if(qcom8gen3_mdt_stage(dst, QCOM8GEN3_ADSP_SPAN, fw, fw_size, ph,
                eh->e_phnum, min_addr) != 0)
        return QCOM8GEN3_ADSP_PAS_INVALID;
    __asm__ volatile("dsb sy" ::: "memory");
    adsp_debug("qcom8gen3 adsp: image staged; PT_LOAD BSS zeroed, gaps preserved\n");

    ret = scm_call(0x205, 1, QCOM8GEN3_ADSP_PAS_ID, 0, 0, 0, &result);
    adsp_debug("qcom8gen3 adsp: auth_and_reset ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == QCOM8GEN3_GPU_PAS_BUSY ? QCOM8GEN3_ADSP_PAS_BUSY :
            QCOM8GEN3_ADSP_PAS_AUTH;
    if(result) return QCOM8GEN3_ADSP_PAS_AUTH;
    booted = 1;
    printf("qcom8gen3 adsp: ADSP authenticated and out of reset\n");

#if QCOM8GEN3_ADSP_TRANSPORT
    (void)adsp_wait_for_ready();

    if(adsp_glink_init() == 0) {
        for(unsigned tries = 0; tries < 100; tries++) {
            (void)adsp_glink_poll();
            if(adsp_glink.version_ack && adsp_glink.open_ack)
                break;
            adsp_delay_ms(20);
        }
        adsp_debug("qcom8gen3 adsp: glink %s cid=%u\n",
                (adsp_glink.version_ack && adsp_glink.open_ack) ?
                "open" : "not open", adsp_glink.cid);
    }
#endif
    return 0;
}
#endif

uint64_t platform_control_adsp(uint64_t op, uint64_t arg, uint64_t size) {
#ifdef QCOM8GEN3_QEMU_TEST
    (void)op; (void)arg; (void)size;
    return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
#else
    if(op == QCOM8GEN3_ADSP_PAS_QUERY) {
        if(arg || size) return QCOM8GEN3_ADSP_PAS_INVALID;
        if(!probed) {
            probe_rc = adsp_probe();
            probed = 1;
        }
        if(probe_rc == QCOM8GEN3_GPU_PAS_BUSY || probe_rc == QCOM8GEN3_GPU_PAS_WAITQ)
            return probe_rc;
        if(probe_rc) return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
        return QCOM8GEN3_ADSP_ABI;
    }

    if(op == QCOM8GEN3_ADSP_PAS_STATUS) {
        uint64_t state = 0;
        if(arg || size) return QCOM8GEN3_ADSP_PAS_INVALID;
        if(booted) state |= QCOM8GEN3_ADSP_STATE_BOOTED;
        return state;
    }

    if(op == QCOM8GEN3_ADSP_PAS_BOOT) {
        void *fw;
        uint32_t capacity = 0;
        if(booted) return 0;
        if(!owned_dma(arg, size, ADSP_MAX_FW, &capacity) ||
                capacity < size + ADSP_META_SIZE)
            return QCOM8GEN3_ADSP_PAS_INVALID;
        fw = (void *)dma_v_addr(arg);
        if(fw == 0) return QCOM8GEN3_ADSP_PAS_INVALID;
        return stage_and_boot(fw, (uint32_t)size, arg, capacity);
    }

    if(op == QCOM8GEN3_ADSP_PAS_STOP) {
        uint64_t result = 0;
        int ret;

        if(arg || size) return QCOM8GEN3_ADSP_PAS_INVALID;
        if(!booted) return 0;
        ret = scm_call(0x206, 1, QCOM8GEN3_ADSP_PAS_ID, 0, 0, 0, &result);
        if(ret != 0 || result != 0) {
            printf("qcom8gen3 adsp: PAS_STOP failed ret=%d result=%llu\n",
                    ret, (unsigned long long)result);
            return QCOM8GEN3_ADSP_PAS_FAILED;
        }
#if QCOM8GEN3_ADSP_TRANSPORT
        memset(&adsp_glink, 0, sizeof(adsp_glink));
        (void)adsp_power_release();
#endif
        booted = 0;
        printf("qcom8gen3 adsp: ADSP stopped\n");
        return 0;
    }

    if(op == QCOM8GEN3_ADSP_BATTMGR_QUERY) {
        if(arg || size) return QCOM8GEN3_ADSP_PAS_INVALID;
        if(!booted) return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
#if QCOM8GEN3_ADSP_TRANSPORT
        return QCOM8GEN3_ADSP_BATTMGR_ABI;
#else
        return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
#endif
    }

    if(op == QCOM8GEN3_ADSP_BATTMGR_CAPACITY) {
        if(arg || size) return QCOM8GEN3_ADSP_PAS_INVALID;
#if QCOM8GEN3_ADSP_TRANSPORT
        uint32_t percent = 0;
        int charging = 0, present = 0;
        if(!booted) return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
        if(adsp_battmgr_query_battery(&percent, &charging, &present) != 0)
            return QCOM8GEN3_ADSP_PAS_FAILED;
        return (percent & QCOM8GEN3_ADSP_BATT_CAP_MASK) |
                (charging ? QCOM8GEN3_ADSP_BATT_CHARGING : 0) |
                (present ? QCOM8GEN3_ADSP_BATT_PRESENT : 0);
#else
        return QCOM8GEN3_ADSP_PAS_UNSUPPORTED;
#endif
    }

    return QCOM8GEN3_ADSP_PAS_INVALID;
#endif
}
