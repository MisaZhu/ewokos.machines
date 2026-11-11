/* Q8B NPU bring-up: SC8280XP NSP0 (PAS id 18).
 *
 * One restricted op boots the signed cDSP image: userspace hands the kernel a
 * DMA buffer holding the board firmware (qccdsp8280.mbn), and the kernel does
 * exactly what qcom_mdt_load() + qcom_q6v5_pas do on Linux:
 *
 *   parse ELF32 -> extract metadata (ELF header + hash segment)
 *   SCM PAS_INIT(18, metadata) -> PAS_MEM_SETUP(carveout, span)
 *   copy relocated segments into the carveout -> PAS_AUTH_AND_RESET(18)
 *
 * Only the fixed NSP0 carveout from the board DT is reachable; no caller
 * supplied SMC function, PAS id or destination address is accepted.
 */
#include <stdint.h>
#include <kernel/proc.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/dma.h>
#include <mm/mmu.h>
#include <kprintf.h>
#include <q8b_npu.h>
#include <q8b_fastrpc_wire.h>
#include <q8b_smp2p_wire.h>
#include <q8b_npu_ipcc.h>
#include <q8b_npu_watchdog.h>
#include <q8b_rpmh_tcs.h>
#include <q8b_qrtr_wire.h>
#include <q8b_mdt_load.h>
#include <q8b_npu_dump.h>
#include "hw_arch.h"
#include "scm-probe.h"
#include "npu_smmu.h"

/* Enable protocol traces explicitly for bring-up. Normal builds retain
 * errors and fault snapshots without logging every GLINK transaction. */
#ifndef Q8B_NPU_DEBUG
#define Q8B_NPU_DEBUG 0
#endif
#define npu_debug(...) do { if(Q8B_NPU_DEBUG) printf(__VA_ARGS__); } while(0)

#define NPU_MAX_FW      (8u * 1024u * 1024u)
#define NPU_MAX_DMA     (32u * 1024u * 1024u) /* official V68 Skel PT_LOAD is > 8 MiB */
#define NPU_META_SIZE   0x4000u
#define NPU_PT_LOAD     1u
#define NPU_SMP2P_DATA_SIZE 0x154u /* 20-byte header + 16 * 20-byte entries */
#define NPU_SMP2P_ITEM_SIZE 0x158u /* SMEM size is 8-byte aligned; 4 pad bytes */
#define NPU_MDT_TYPE_MASK  0x07000000u
#define NPU_MDT_TYPE_HASH  0x02000000u

#ifndef Q8B_QEMU_TEST
extern void q8b_scm_smc(uint64_t regs[8]);

static int scm_call(uint32_t fn, uint32_t info, uint64_t a, uint64_t b,
        uint64_t c, int fast, uint64_t *result) {
    uint64_t input[8] = { (fast ? 0xc2000000u : 0x42000000u) | fn,
        info, a, b, c, 0, 0, 0 };
    uint64_t regs[8];
    unsigned n;

    for(n = 0; n < 1024; n++) {
        for(unsigned i = 0; i < 8; i++) regs[i] = input[i];
        __asm__ volatile("dsb sy" ::: "memory");
        q8b_scm_smc(regs);
        __asm__ volatile("dsb sy" ::: "memory");
        if(regs[0] != 1) break;
        input[0] = 1;
        input[6] = regs[6];
    }
    if(regs[0]) printf("q8b NPU SCM fn=%x a0=%llx a1=%llx a2=%llx\n", fn,
            (unsigned long long)regs[0], (unsigned long long)regs[1],
            (unsigned long long)regs[2]);
    if((int64_t)regs[0] == -12) return Q8B_GPU_PAS_BUSY;
    if(regs[0] == 2) return Q8B_GPU_PAS_WAITQ;
    if(regs[0] != 0) return Q8B_GPU_PAS_FAILED;
    *result = regs[1];
    return 0;
}
#endif

/* Kernel-only Normal-NC windows. */
#define Q8B_NSP_WIN 0x5000000000ull
#define Q8B_SMEM_PHY 0x80900000ull
#define Q8B_SMEM_SIZE 0x200000ull
#define Q8B_SMEM_WIN 0x5020000000ull


static int probed;
static int probe_rc;
static ewokos_addr_t meta_pa;
static void *meta_va;
static int32_t win_pid = -1;
static int booted;
static int pas_prepared;
static int pas_init_done;
static int pas_mem_done;
static int pas_auth_done;
/* Do not repeat the observed timeout -> restart -> board-reset sequence.
 * Successful SCM shutdown permits DMA cleanup, but has not proved the
 * crashed transport safe to start again. Only a kernel reboot clears this. */
static int npu_recovery_required;
/* Publish only after PAS_STOP succeeds. Never read a running DSP's RAM. */
static int npu_dump_available;

static int map_in_caller_attr(ewokos_addr_t va, ewokos_addr_t pa,
        uint32_t size, uint32_t attr) {
    proc_t *p = proc_get_proc(get_current_proc());
    page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;

    if(map_pages_size(_kernel_info.kernel_vm, va, pa, size,
                AP_RW_D, attr) != 0)
        return -1;
    if(pvm != 0 && pvm != _kernel_info.kernel_vm &&
            map_pages_size(pvm, va, pa, size, AP_RW_D, attr) != 0)
        return -1;
    flush_tlb();
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int map_in_caller(ewokos_addr_t va, ewokos_addr_t pa, uint32_t size) {
    return map_in_caller_attr(va, pa, size, PTE_ATTR_SYS_DMA);
}

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
} npu_elf32_hdr;

typedef q8b_mdt_phdr npu_elf32_phdr;

#ifdef Q8B_QEMU_TEST
static int nsp_probe(void) { return Q8B_NPU_PAS_UNSUPPORTED; }
#else
static int nsp_probe(void) {
    int ret = q8b_scm_probe(scm_call);

    npu_debug("q8b npu: scm probe ret=%d\n", ret);
    if(ret) return ret;
    return 0;
}

/* ---- minimal RPMh power-domain vote (cmd-db + apps RSC TCS) ---------- */
#define NPU_CMDB_WIN  0x5020200000ull
#define NPU_RSC_WIN   0x5020300000ull
#define NPU_CMDB_PHY  0x80860000ull
#define NPU_CMDB_SIZE 0x20000u
#define NPU_RSC_PHY   0x18220000ull
#define NPU_RSC_SIZE  0x10000u
#define NPU_TCS_OFF   Q8B_RSC_TCS_OFFSET
#define NPU_RSC_IRQ_WIN (NPU_RSC_WIN + NPU_TCS_OFF)
#define NPU_TCS_STRIDE 672u
#define NPU_IRQ_ENABLE Q8B_RSC_IRQ_ENABLE
#define NPU_IRQ_STATUS Q8B_RSC_IRQ_STATUS
#define NPU_IRQ_CLEAR  Q8B_RSC_IRQ_CLEAR
#define NPU_AMC_ENABLE (1u << 16)
#define NPU_AMC_TRIGGER (1u << 24)
#define NPU_WRITE_RESP_MSGID 0x00010108u
#define NPU_BCM_MAX_VOTE     0x60003fffu /* commit + valid + peak=0x3fff */

static struct {
    uint32_t control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data,
             cmd_status, cmd_stride;
    int ready;
    uint32_t completions, previous_compl, compl_without_irq;
} npu_rsc = { 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20, 0, 0, 0, 0 };

static uint8_t cdb8(uint32_t off) {
    return *(volatile uint8_t *)(NPU_CMDB_WIN + off);
}
static uint16_t cdb16(uint32_t off) {
    return (uint16_t)(cdb8(off) | ((uint16_t)cdb8(off + 1) << 8));
}
static uint32_t cdb32(uint32_t off) {
    return *(volatile uint32_t *)(NPU_CMDB_WIN + off);
}
static int cdb_find(const char *id, uint32_t *addr, uint32_t *aux_off,
        uint32_t *aux_len) {
    uint32_t i;
    if(cdb32(4) != 0x0c0330dbu) return -1;
    for(i = 0; i < 8; i++) {
        uint32_t rsc = 8 + i * 16;
        uint32_t slave = cdb16(rsc), hdr, data, count, entries, j;
        if(slave == 0) break;
        hdr = cdb16(rsc + 2); data = cdb16(rsc + 4); count = cdb16(rsc + 6);
        entries = 144 + hdr;
        for(j = 0; j < count; j++) {
            uint32_t e = entries + j * 24;
            uint32_t k;
            for(k = 0; k < 8; k++) {
                uint8_t c = (uint8_t)id[k];
                if(cdb8(e + k) != c) break;
                if(c == 0) { k = 8; break; }
            }
            if(k < 8) continue;
            if(addr) *addr = cdb32(e + 16);
            if(aux_off) *aux_off = 144 + data + cdb16(e + 22);
            if(aux_len) *aux_len = cdb16(e + 20);
            return 0;
        }
    }
    return -1;
}
static uint32_t rsc_reg(uint32_t tcs, uint32_t reg) {
    return *(volatile uint32_t *)(NPU_RSC_WIN + NPU_TCS_OFF +
            tcs * NPU_TCS_STRIDE + reg);
}
static void rsc_wr(uint32_t tcs, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(NPU_RSC_WIN + NPU_TCS_OFF +
            tcs * NPU_TCS_STRIDE + reg) = val;
}
static uint32_t rsc_cmd(uint32_t tcs, uint32_t cmd, uint32_t reg) {
    return NPU_TCS_OFF + tcs * NPU_TCS_STRIDE + cmd * npu_rsc.cmd_stride + reg;
}
static int rsc_wr_sync(uint32_t tcs, uint32_t reg, uint32_t val) {
    rsc_wr(tcs, reg, val);
    for(int i = 0; i < 100000; i++) {
        if(rsc_reg(tcs, reg) == val) return 0;
        __asm__ volatile("yield" ::: "memory");
    }
    return -1;
}
static uint32_t rsc_read32(void *context, uint32_t off) {
    (void)context;
    return *(volatile uint32_t *)(NPU_RSC_WIN + off);
}
static void rsc_relax(void *context) {
    (void)context;
    __asm__ volatile("yield" ::: "memory");
}
static int rpmh_write(uint32_t address, uint32_t data) {
    const uint32_t tcs = 0, irq = 1u;
    uint32_t control;
    int i;
    if(!npu_rsc.ready) return -2;
    control = rsc_reg(tcs, npu_rsc.control) & ~NPU_AMC_TRIGGER;
    if(rsc_wr_sync(tcs, npu_rsc.control, control) != 0) return -3;
    control &= ~NPU_AMC_ENABLE;
    if(rsc_wr_sync(tcs, npu_rsc.control, control) != 0) return -4;
    if(rsc_wr_sync(tcs, npu_rsc.cmd_enable, 0) != 0) return -5;
    *(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_CLEAR) = irq;
    __asm__ volatile("dsb sy" ::: "memory");
    /* A fresh TCS IRQ, rather than a retained CMD_STATUS.COMPL, completes
     * this response-required write. Confirm the previous IRQ was cleared. */
    for(i = 0; i < 100000; i++)
        if(!(*(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_STATUS) & irq))
            break;
    if(i == 100000) return -9;
    *(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_ENABLE) =
            *(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_ENABLE) | irq;
    *(volatile uint32_t *)(NPU_RSC_WIN + rsc_cmd(tcs, 0,
                npu_rsc.cmd_msgid)) = NPU_WRITE_RESP_MSGID;
    *(volatile uint32_t *)(NPU_RSC_WIN + rsc_cmd(tcs, 0,
                npu_rsc.cmd_addr)) = address;
    *(volatile uint32_t *)(NPU_RSC_WIN + rsc_cmd(tcs, 0,
                npu_rsc.cmd_data)) = data;
    if(rsc_wr_sync(tcs, npu_rsc.cmd_enable, 1) != 0) return -6;
    if(rsc_wr_sync(tcs, npu_rsc.control, NPU_AMC_ENABLE) != 0) return -7;
    if(*(volatile uint32_t *)(NPU_RSC_WIN +
                rsc_cmd(tcs, 0, npu_rsc.cmd_status)) & (1u << 16))
        npu_rsc.previous_compl++;
    rsc_wr(tcs, npu_rsc.control, NPU_AMC_ENABLE | NPU_AMC_TRIGGER);
    const q8b_rpmh_io_t io = { 0, rsc_read32, rsc_relax };
    if(q8b_rpmh_wait_irq(&io, rsc_cmd(tcs, 0, npu_rsc.cmd_status),
                irq, 1000000, &npu_rsc.compl_without_irq) == 0) {
        npu_rsc.completions++;
        uint32_t c = rsc_reg(tcs, npu_rsc.control) & ~NPU_AMC_TRIGGER;
        if(rsc_wr_sync(tcs, npu_rsc.control, c) != 0 ||
                rsc_wr_sync(tcs, npu_rsc.control, 0) != 0 ||
                rsc_wr_sync(tcs, npu_rsc.cmd_enable, 0) != 0)
            return -10;
        *(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_CLEAR) = irq;
        return 0;
    }
    printf("q8b npu: RPMh fresh IRQ timeout addr=%x data=%x cmd=%x irq=%x\n",
            address, data, *(volatile uint32_t *)(NPU_RSC_WIN +
                rsc_cmd(tcs, 0, npu_rsc.cmd_status)),
            *(volatile uint32_t *)(NPU_RSC_IRQ_WIN + NPU_IRQ_STATUS));
    return -8;
}
static int rpmh_vote(const char *name) {
    uint32_t addr = 0, aux = 0, len = 0, i, count, corner;
    if(cdb_find(name, &addr, &aux, &len) != 0 || addr == 0) {
        printf("q8b npu: rpmh %s not in cmd-db\n", name);
        return -1;
    }
    count = len / 2;
    for(i = 1; i < count; i++) {
        if(cdb16(aux + i * 2) == 0) { count = i; break; }
    }
    if(count == 0) count = 1;
    corner = count - 1;
    if(rpmh_write(addr, corner) != 0) {
        printf("q8b npu: rpmh %s vote failed addr=%x\n", name, addr);
        return -1;
    }
    npu_debug("q8b npu: rpmh %s addr=%x corner=%u ok\n", name, addr, corner);
    return 0;
}

/* qcom_q6v5_prepare() calls icc_set_bw(path, 0, UINT_MAX) before PAS.  On
 * SC8280XP the nspa_noc path is represented by the NSA0 (fabric/link) and
 * NSA1 (CDSP master) BCMs.  Without these AMC votes the cDSP can authenticate
 * successfully but cannot fetch its image or reach the SMEM doorbells. */
static int rpmh_bcm_max(const char *name) {
    uint32_t addr = 0;
    if(cdb_find(name, &addr, 0, 0) != 0 || addr == 0) {
        printf("q8b npu: rpmh BCM %s not in cmd-db\n", name);
        return -1;
    }
    if(rpmh_write(addr, NPU_BCM_MAX_VOTE) != 0) {
        printf("q8b npu: rpmh BCM %s vote failed addr=%x\n", name, addr);
        return -1;
    }
    npu_debug("q8b npu: rpmh BCM %s addr=%x max ok\n", name, addr);
    return 0;
}
static int nsp_power_vote(void) {
    uint32_t id;
    if(map_in_caller(NPU_CMDB_WIN, NPU_CMDB_PHY, NPU_CMDB_SIZE) != 0 ||
            map_in_caller_attr(NPU_RSC_WIN, NPU_RSC_PHY, NPU_RSC_SIZE,
                PTE_ATTR_DEV) != 0) {
        printf("q8b npu: rpmh map failed\n");
        return -1;
    }
    id = *(volatile uint32_t *)NPU_RSC_WIN;
    if(((id >> 16) & 0xff) >= 3) {
        npu_rsc.control = 0x24; npu_rsc.status = 0x28;
        npu_rsc.cmd_enable = 0x2c; npu_rsc.cmd_msgid = 0x34;
        npu_rsc.cmd_addr = 0x38; npu_rsc.cmd_data = 0x3c;
        npu_rsc.cmd_status = 0x40; npu_rsc.cmd_stride = 24;
    }
    npu_rsc.ready = 1;
    npu_debug("q8b npu: rpmh rsc id=%x\n", id);
    /* qcom_q6v5_prepare() enables the RPMh XO clock before PAS metadata is
     * submitted.  The cDSP can authenticate without it, but its early SMEM
     * and GLINK initialization may then stop before allocating item 480. */
    if(rpmh_vote("xo.lvl") != 0 || rpmh_vote("nsp.lvl") != 0) return -1;
    /* sc8280xp_nsp0's Linux PAS resource keeps the CX proxy domain enabled
     * while NSP0 is running.  The GPU may already have a CX vote, but that is
     * not a lifetime guarantee and must not be relied upon for NPU bring-up. */
    if(rpmh_vote("cx.lvl") != 0 || rpmh_vote("mxc.lvl") != 0) return -1;
    /* Linux's qcom_q6v5_prepare() holds this path at UINT_MAX until the
     * handover IRQ. Keep both BCMs active for this first port. These shared
     * interconnect votes require aggregation before they can be released. */
    if(rpmh_bcm_max("NSA0") != 0 || rpmh_bcm_max("NSA1") != 0) return -1;
    npu_debug("q8b npu: RPMh fresh IRQs=%u previous_compl=%u compl_without_irq=%u\n",
            npu_rsc.completions, npu_rsc.previous_compl, npu_rsc.compl_without_irq);
    return 0;
}

static int nsp_power_release(void) {
    uint32_t addr = 0;
    /* sc8280xp_nsp0_resource has a single "nsp" proxy domain. Mainline
     * qcom_pas_handover()/rpmhpd_power_off() withdraw its AP vote. Retaining
     * the maximum vote across PAS shutdown can prevent domain collapse.
     * Only NSP is exclusive here: CX/MXC/XO and BCM votes are shared with
     * other clients and cannot be cleared without vote aggregation. */
    if(cdb_find("nsp.lvl", &addr, 0, 0) != 0 || !addr ||
            rpmh_write(addr, 0) != 0) {
        printf("q8b npu: rpmh nsp.lvl release failed\n");
        return -1;
    }
    npu_debug("q8b npu: rpmh nsp.lvl addr=%x corner=0 released\n", addr);
    return 0;
}

static void npu_delay_ms(uint32_t ms) {
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    do {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    } while ((now - start) * 1000ull < freq * ms);
}

static void npu_ipcc_enable(uint32_t signal) {
    volatile uint32_t *ipcc = (volatile uint32_t *)(MMIO_BASE + 0x408000);
    uint32_t hwirq = (6u << 16) | signal; /* IPCC_CLIENT_CDSP */
    uint32_t config = ipcc[0x08 / 4];
    /* Match qcom-ipcc: boot firmware may leave clear-on-read enabled. */
    if(config & 1u) ipcc[0x08 / 4] = config & ~1u;
    ipcc[0x14 / 4] = hwirq;
}

/* Completion belongs to this authentication attempt. Mailbox delivery plus
 * valid state is necessary, but is not a proof of every transport epoch. */
static int npu_boot_smp2p_notified;
static uint32_t npu_ipcc_recv(void *data) {
    (void)data;
    return *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x10);
}
static void npu_ipcc_clear(void *data, uint32_t id) {
    (void)data;
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x1c) = id;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t npu_ipcc_poll(void) {
    uint32_t signals = q8b_npu_ipcc_take(npu_ipcc_recv, npu_ipcc_clear, 0);
    if (signals & 4u) npu_boot_smp2p_notified = 1;
    return signals;
}

/* sc8280xp.dtsi routes NSP0 watchdog to GIC_SPI578, hence INTID610.
 * SMP2P fatal is a different interrupt and need not change on a watchdog.
 * Read only: never acknowledge, enable, reroute or clear another handler's
 * interrupt. A zero may also reflect security filtering, not DSP health. */
static q8b_npu_wdog_state_t npu_watchdog;
static void npu_dump_crash_reason(void);
static uint32_t npu_watchdog_read32(void *context, uint32_t offset) {
    (void)context;
    volatile uint32_t *gicd = (volatile uint32_t *)(MMIO_BASE +
            Q8B_GIC_WIN_OFF + Q8B_GICD_PHY - Q8B_GIC_WIN_PHY);
    return gicd[offset / 4];
}
static void npu_watchdog_snapshot(const char *phase) {
    q8b_npu_wdog_sample_t sample;
    if (q8b_npu_wdog_read(npu_watchdog_read32, 0, &sample)) {
        printf("q8b npu: watchdog %s GIC unavailable\n", phase);
        return;
    }
    printf("q8b npu: watchdog %s intid=%u group=%u enabled=%u pending=%u active=%u\n",
            phase, Q8B_NPU_WDOG_INTID, sample.group, sample.enabled, sample.pending, sample.active);
}
static int npu_watchdog_prepare(void) {
    q8b_npu_wdog_sample_t sample;
    int rc = q8b_npu_wdog_read(npu_watchdog_read32, 0, &sample);
    if (q8b_npu_wdog_arm(&npu_watchdog, rc ? 0 : &sample)) {
        npu_recovery_required = 1;
        printf("q8b npu: watchdog not clean before PAS_INIT; board reset required\n");
        npu_watchdog_snapshot("prepare-rejected");
        return Q8B_NPU_PAS_FAILED;
    }
    return 0;
}
static int npu_watchdog_refresh(void) {
    q8b_npu_wdog_sample_t sample;
    int before = npu_watchdog.tripped;
    if (q8b_npu_wdog_read(npu_watchdog_read32, 0, &sample) == 0)
        (void)q8b_npu_wdog_observe(&npu_watchdog, &sample);
    if (npu_watchdog.tripped) {
        npu_recovery_required = 1;
        if (!before) {
            printf("q8b npu: NSP0 watchdog asserted; transport quarantined\n");
            npu_watchdog_snapshot("asserted");
            npu_dump_crash_reason();
        }
    }
    return npu_watchdog.tripped;
}

static void npu_boot_notify_arm(void) {
    uint32_t stale = npu_ipcc_poll();
    npu_boot_smp2p_notified = 0;
    npu_debug("q8b npu: boot notify armed drained=%x next=%x\n", stale, npu_ipcc_recv(0));

}

/* Some production CDSP images insert private entries with a non-zero
 * padding_hdr, and a few boot ROMs leave stale bytes between entries.  Walk
 * by bytes when looking up a known item instead of assuming a zero-padding
 * linked list. */
static uint8_t *smem_find_private_item(uint8_t *ph, uint32_t free_off,
        uint16_t want) {
    if(free_off < 32 || free_off > Q8B_SMEM_SIZE ||
            free_off > *(const uint32_t *)(ph + 8))
        return 0;
    for(uint32_t off = 32; off + 16 <= free_off; off += 4) {
        uint8_t *e = ph + off;
        uint16_t canary = *(uint16_t *)(e + 0);
        uint16_t item = *(uint16_t *)(e + 2);
        uint32_t size = *(uint32_t *)(e + 4);
        uint16_t pad_hdr = *(uint16_t *)(e + 10);
        uint32_t next;
        if(canary != 0xa5a5 || size > 0x10000 || pad_hdr > 0x1000)
            continue;
        next = off + 16 + pad_hdr + ((size + 7u) & ~7u);
        if(next > free_off)
            continue;
        if(item == want)
            return e;
    }
    return 0;
}

/* qcom_q6v5_pas's sc8280xp_nsp0_resource uses SMEM item 601 for fatal
 * messages. Firmware v12 keeps host-any items in the global partition,
 * rather than the legacy header TOC. Read it before PAS resets the DSP. */
static void npu_dump_crash_reason(void) {
    uint8_t *s = (uint8_t *)Q8B_SMEM_WIN;
    static const uint32_t offsets[] = {0x1000u, 0x1af000u};
    for(unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        uint8_t *ph = s + offsets[i];
        uint32_t size = *(const uint32_t *)(ph + 8);
        uint32_t free_off = *(const uint32_t *)(ph + 12);
        if(size > Q8B_SMEM_SIZE - offsets[i] || free_off > size) continue;
        const uint8_t *e = smem_find_private_item(ph, free_off, 601);
        if(!e) continue;
        uint32_t bytes = *(const uint32_t *)(e + 4);
        uint16_t padding = *(const uint16_t *)(e + 8);
        uint16_t header = *(const uint16_t *)(e + 10);
        if(padding > bytes || !bytes || header > 0x1000u) continue;
        const uint8_t *message = e + 16 + header;
        if(!message[0]) continue;
        char text[256];
        unsigned n = 0;
        while(n < sizeof(text) - 1 && n < bytes - padding && message[n]) {
            uint8_t c = message[n];
            text[n++] = c >= 32 && c < 127 ? (char)c : ' ';
        }
        text[n] = 0;
        printf("q8b npu: SMEM601 crash reason: %s\n", text);
    }
}

/* Linux qcom_q6v5 exposes these as four inbound IRQs on SMP2P item 94.
 * EwokOS polls the same wire values because it has no remoteproc IRQ domain;
 * the protocol still tells us whether FastRPC may touch the DSP. */
static uint32_t npu_smp2p_state;
/* SMP2P belongs to the AP driver lifetime, unlike the per-boot GLINK edge.
 * Remember only an item initialized by this kernel, never a UEFI leftover. */
static uint8_t *npu_smp2p_master;

static int npu_smp2p_read_remote(uint32_t values[4]) {
    uint8_t *s = (uint8_t *)Q8B_SMEM_WIN;
    uint8_t *ph = s + 0x1af000;
    uint32_t fU = *(volatile uint32_t *)(ph + 12);
    const uint8_t *it;
    const uint8_t *d;
    uint32_t size, bits;
    uint16_t padding, padding_hdr;

    if(!pas_prepared && !booted)
        return -1;
    it = smem_find_private_item(ph, fU, Q8B_SMP2P_IN_ITEM);
    if(!it)
        return -1;
    size = *(const uint32_t *)(it + 4);
    padding = *(const uint16_t *)(it + 8);
    padding_hdr = *(const uint16_t *)(it + 10);
    if(padding > size)
        return -1;
    d = it + 16 + padding_hdr;
    if(size - padding < 20 || *(const uint32_t *)d != 0x504d5324u ||
            (d[4] != 1 && d[4] != 2))
        return -1;
    /* Match qcom_smp2p_negotiate(): production NSP0 advertises v1, while
     * AP initially offers v2. Downgrade and kick before waiting for entries. */
    const uint8_t *out_item = smem_find_private_item(ph, fU, Q8B_SMP2P_OUT_ITEM);
    if(out_item) {
        uint8_t *out = (uint8_t *)out_item + 16 +
                *(const uint16_t *)(out_item + 10);
        int notify = 0;
        if(out[4] > d[4]) {
            out[4] = d[4];
            npu_debug("q8b npu: SMP2P negotiated version %u\n", d[4]);
            notify = 1;
        }
        if(out[4] == d[4]) {
            out[5] &= d[5]; out[6] &= d[6]; out[7] &= d[7];
            if((out[5] & 1u) &&
                    ((d[16] & 1u) != ((out[16] >> 1) & 1u))) {
                out[16] ^= 2u; /* mirror remote RESTART_DONE as RESTART_ACK */
                notify = 1;
            }
        }
        if(notify) {
            __asm__ volatile("dsb sy" ::: "memory");
            *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
            __asm__ volatile("dsb sy" ::: "memory");
        }
    }
    if(q8b_smp2p_slave_bits(d, size - padding, &bits) != 0)
        return -1;
    /* All four IRQs are bits of ONE named slave-kernel value, not values
     * of four separate entries. */
    for(unsigned i = 0; i < 4; i++)
        values[i] = (bits >> i) & 1u;
    return 0;
}

static uint32_t npu_smp2p_refresh_state(void) {
    uint32_t old = npu_smp2p_state;
    uint32_t v[4];

    (void)npu_ipcc_poll();

    npu_smp2p_state &= ~(Q8B_NPU_STATE_REMOTE | Q8B_NPU_STATE_READY |
            Q8B_NPU_STATE_HANDOVER | Q8B_NPU_STATE_FATAL |
            Q8B_NPU_STATE_STOP_ACK);
    if(npu_smp2p_read_remote(v) == 0) {
        npu_smp2p_state |= Q8B_NPU_STATE_REMOTE;
        if(v[0]) npu_smp2p_state |= Q8B_NPU_STATE_FATAL;
        if(v[1]) npu_smp2p_state |= Q8B_NPU_STATE_READY;
        if(v[2]) npu_smp2p_state |= Q8B_NPU_STATE_HANDOVER;
        if(v[3]) npu_smp2p_state |= Q8B_NPU_STATE_STOP_ACK;
    }
    if(npu_watchdog_refresh()) npu_smp2p_state |= Q8B_NPU_STATE_FATAL;
    if(npu_smp2p_state != old)
        npu_debug("q8b npu: NSP state %08x -> %08x\n", old,
                npu_smp2p_state);
    return npu_smp2p_state;
}

static int npu_smp2p_request_stop(void) {
    uint8_t *s = (uint8_t *)Q8B_SMEM_WIN;
    uint8_t *ph = s + 0x1af000;
    uint32_t fU = *(volatile uint32_t *)(ph + 12);
    const uint8_t *it = smem_find_private_item(ph, fU, Q8B_SMP2P_OUT_ITEM);
    uint8_t *d;
    uint32_t *stop_value = 0;

    if(!it)
        return -1;
    d = (uint8_t *)(it + 16);
    if(*(uint32_t *)(d + 0) != 0x504d5324u)
        return -1;
    {
        uint16_t valid = *(uint16_t *)(d + 14);
        if(valid > 16) valid = 16;
        for(uint16_t i = 0; i < valid; i++) {
            uint8_t *e = d + 20 + i * 20;
            int match = 1;
            for(unsigned k = 0; k < 13; k++)
                if(e[k] != (uint8_t)"master-kernel"[k]) match = 0;
            if(match) {
                stop_value = (uint32_t *)(e + 16);
                break;
            }
        }
    }
    if(!stop_value)
        return -1;
    *stop_value = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
    __asm__ volatile("dsb sy" ::: "memory");
    for(unsigned i = 0; i < 50; i++) {
        uint32_t state = npu_smp2p_refresh_state();
        if(state & (Q8B_NPU_STATE_STOP_ACK | Q8B_NPU_STATE_FATAL))
            break;
        npu_delay_ms(10);
    }
    *stop_value = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
    __asm__ volatile("dsb sy" ::: "memory");
    return (npu_smp2p_state & Q8B_NPU_STATE_STOP_ACK) ? 0 : -1;
}

static void smem_prepare(void) {
    uint8_t *s;
    uint32_t init;
    const uint8_t *gph;
    uint32_t gsize, gfree;

    if(map_in_caller(Q8B_SMEM_WIN, Q8B_SMEM_PHY, Q8B_SMEM_SIZE) != 0) {
        printf("q8b npu: smem_prepare map failed\n");
        return;
    }
    s = (uint8_t *)Q8B_SMEM_WIN;
    init = *(uint32_t *)(s + 0xc0);
    npu_debug("q8b npu: smem header init=%u free=%x avail=%x\n", init,
            *(uint32_t *)(s + 0xc4), *(uint32_t *)(s + 0xc8));
    if(init != 1) {
        gph = s + 0x1000;
        gsize = *(const uint32_t *)(gph + 8);
        gfree = *(const uint32_t *)(gph + 12);
        if(gsize == 0 || gfree > gsize) { gsize = 0x136000; gfree = 0x20; }
        *(uint32_t *)(s + 0xc0) = 1;
        *(uint32_t *)(s + 0xc4) = 0x1000 + gfree;
        *(uint32_t *)(s + 0xc8) = gsize - gfree;
        *(uint32_t *)(s + 0xcc) = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        npu_debug("q8b npu: smem header restored free=%x avail=%x\n",
                0x1000 + gfree, gsize - gfree);
    }
    /* Prepare the AP-owned SMP2P edge in the 0/5 partition.  GLINK item 480
     * remains remote-owned and is only inspected below after the DSP starts. */
    {
        uint8_t *ph = s + 0x1af000;
        uint32_t fU = *(uint32_t *)(ph + 12);
        uint32_t fC = *(uint32_t *)(ph + 16);
        /* Allocate the host SMP2P outbound item (432) so the DSP sees a valid
         * master-kernel entry before it brings up GLINK. */
        {
            int have_out = 0;
            const uint8_t *w = ph + 32;
            const uint8_t *we = ph + fU;
            while(w + 16 <= we) {
                uint16_t canary = *(const uint16_t *)(w + 0);
                uint16_t item = *(const uint16_t *)(w + 2);
                uint32_t isz = *(const uint32_t *)(w + 4);
                if(canary != 0xa5a5 || isz > 0x10000) break;
                if(item == Q8B_SMP2P_OUT_ITEM) have_out = 1;
                w += 16 + *(const uint16_t *)(w + 10) +
                        ((isz + 7) & ~7u);
            }
            if(have_out) {
                const uint8_t *w2 = ph + 32;
                while(w2 + 16 <= ph + fU) {
                    uint16_t canary = *(const uint16_t *)(w2 + 0);
                    uint16_t item = *(const uint16_t *)(w2 + 2);
                    uint32_t isz = *(const uint32_t *)(w2 + 4);
                    if(canary != 0xa5a5 || isz > 0x10000) break;
                    if(item == Q8B_SMP2P_OUT_ITEM) {
                        if(npu_smp2p_master == (uint8_t *)(w2 + 16) &&
                                isz == NPU_SMP2P_ITEM_SIZE) {
                            npu_debug("q8b npu: preserving AP SMP2P version=%u flags=%x\n",
                                    npu_smp2p_master[4], npu_smp2p_master[16]);
                            break;
                        }
                        uint8_t *d;
                        uint32_t old_size = isz;
                        uint32_t next = (uint32_t)(w2 - ph) + 16 +
                                ((old_size + 7u) & ~7u);
                        /* Older development images allocated 0x150 bytes,
                         * omitting the four-byte flags field.  Grow that
                         * entry in-place before the DSP is released. */
                        if(old_size < NPU_SMP2P_ITEM_SIZE) {
                            uint32_t grow = NPU_SMP2P_ITEM_SIZE - old_size;
                            if(*(uint32_t *)(ph + 12) + grow > fC) {
                                printf("q8b npu: SMP2P 432 cannot grow old=%x\n",
                                        old_size);
                                break;
                            }
                            for(uint32_t j = *(uint32_t *)(ph + 12);
                                    j > next; j--)
                                ph[j + grow - 1] = ph[j - 1];
                            *(uint32_t *)(ph + 12) += grow;
                            isz = NPU_SMP2P_ITEM_SIZE;
                        }
                        *(uint32_t *)(w2 + 4) = NPU_SMP2P_ITEM_SIZE;
                        *(uint16_t *)(w2 + 8) = NPU_SMP2P_ITEM_SIZE - NPU_SMP2P_DATA_SIZE;
                        *(uint16_t *)(w2 + 10) = 0;
                        *(uint32_t *)(w2 + 12) = 0;
                        d = (uint8_t *)(w2 + 16);
                        q8b_smp2p_init_master(d, NPU_SMP2P_ITEM_SIZE);
                        __asm__ volatile("dsb sy" ::: "memory");
                        d[4] = 2; /* publish version last, as Linux does */
                        __asm__ volatile("dsb sy" ::: "memory");
                        npu_smp2p_master = d;
                        npu_debug("q8b npu: patched SMP2P item 432 master-kernel\n");
                        break;
                    }
                    w2 += 16 + *(const uint16_t *)(w2 + 10) +
                            ((isz + 7) & ~7u);
                }
            }
            if(!have_out && fU + 16 + NPU_SMP2P_ITEM_SIZE <= fC) {
                uint8_t *e = ph + fU;
                uint8_t *d = e + 16;
                *(uint16_t *)(e + 0) = 0xa5a5;
                *(uint16_t *)(e + 2) = Q8B_SMP2P_OUT_ITEM;
                *(uint32_t *)(e + 4) = NPU_SMP2P_ITEM_SIZE;
                *(uint16_t *)(e + 8) = NPU_SMP2P_ITEM_SIZE - NPU_SMP2P_DATA_SIZE;
                *(uint16_t *)(e + 10) = 0;
                *(uint32_t *)(e + 12) = 0;
                q8b_smp2p_init_master(d, NPU_SMP2P_ITEM_SIZE);
                __asm__ volatile("dsb sy" ::: "memory");
                d[4] = 2;
                __asm__ volatile("dsb sy" ::: "memory");
                npu_smp2p_master = d;
                *(uint32_t *)(ph + 12) = fU + 16 + NPU_SMP2P_ITEM_SIZE;
                fU += 16 + NPU_SMP2P_ITEM_SIZE;
                __asm__ volatile("dsb sy" ::: "memory");
                npu_debug("q8b npu: created SMP2P item 432\n");
            }
        }
        /* Item 480 is the remote-owned RX FIFO.  Linux allocates only 478
         * and 479 here and obtains 480 with qcom_smem_get(); creating it from
         * the AP side races the DSP allocator and leaves the edge unusable. */
    }
}

#define NPU_GLINK_VERSION       1u
/* Match qcom_glink_native: bit 0 is intent reuse.  Advertising the
 * intentless bit (or unknown private bits) changes the DSP's flow-control
 * mode and suppresses the INTENT exchange required by FastRPC. */
#define NPU_GLINK_FEATURES      1u
#define NPU_GLINK_FIFO_RESERVE  16u
#define NPU_GLINK_FIFOMAX       16384u
#define NPU_FRPC_CHANNEL        "fastrpcglink-apps-dsp"
#define NPU_FRPC_INTENT_SIZE    16384u
#define NPU_FRPC_LOCAL_INTENT_SIZE 1024u
#define NPU_FRPC_LOCAL_INTENT_COUNT 5u
#define NPU_FRPC_REMOTE_REQ_SIZE 16384u
#define NPU_FRPC_RX_MAX         65536u

/* Multiple outstanding requests let a listener answer reverse file RPCs
 * while a normal DSP invoke waits.  Syscalls remain serialized and bounded. */
#define NPU_FRPC_PENDING_MAX 4u
typedef struct {
    uint64_t ctx, pa;
    uint64_t maps;
    uint32_t size, handle, sc;
    int32_t retval;
    int done;
    uint64_t submitted;
} npu_frpc_pending_t;

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
    /* A remote intent is unavailable between the last TX_DATA frame and
     * RX_DONE(_W_REUSE).  Keep its iid while waiting so a late reuse
     * notification can restore it, but never let a new invoke consume it
     * speculatively. */
    int remote_wait_done;
    uint32_t local_iid;
    uint32_t local_iid_count;
    uint32_t rx_len;
    uint8_t rx_buf[NPU_FRPC_RX_MAX];
    npu_frpc_pending_t pending[NPU_FRPC_PENDING_MAX];
    struct { uint64_t ctx, tick; int32_t rv; } responses[16];
    uint32_t response_count;
    uint32_t listener_root_submits;
    int metadata_logged;
    int version_sent;
    int version_ack;
    int open_ack;
    int open_sent;
    int ready;
    struct {
        uint16_t cid, remote_cid;
        uint32_t iid, isize, rx_len, packets;
        int open_ack, valid, wait_done, hello_sent;
        uint32_t ssctl_node, ssctl_port;
        uint16_t ssctl_txn;
        int shutdown_pending, shutdown_response;
        int32_t shutdown_result;
        uint8_t rx[4096];
    } qrtr;
} npu_glink_state_t;

static npu_glink_state_t npu_glink;
/* Kernel-lifetime sequence: glink_ping/PAS_STOP clear npu_glink, but
 * queued responses may survive the DSP/channel restart. Never reuse ctx. */
static uint32_t npu_frpc_sequence;

static uint64_t npu_frpc_ticks(void) {
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}

/* Stop-time observation only. Never drain a late response, acknowledge an
 * intent, or change FIFO ownership while diagnosing an incomplete invoke. */
static void glink_dump_failure_fifo(void) {
    uint64_t frequency, now = npu_frpc_ticks();
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    for(unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++) {
        const npu_frpc_pending_t *p = &npu_glink.pending[i];
        if(p->ctx && frequency)
            printf("q8b npu: invoke age ctx=%llx us=%llu done=%d\n",
                    (unsigned long long)p->ctx,
                    (unsigned long long)((now - p->submitted) * 1000000u / frequency), p->done);
    }
    uint32_t count = npu_glink.response_count;
    uint32_t begin = count > 16u ? count - 16u : 0;
    for(uint32_t i = begin; i < count && frequency; i++) {
        unsigned slot = i % 16u;
        printf("q8b npu: recent response ctx=%llx rv=%d age_us=%llu\n",
                (unsigned long long)npu_glink.responses[slot].ctx,
                npu_glink.responses[slot].rv,
                (unsigned long long)((now - npu_glink.responses[slot].tick) * 1000000u / frequency));
    }
    if(!npu_glink.desc || !npu_glink.rx || !npu_glink.len) return;
    uint32_t head = npu_glink.desc[3], tail = npu_glink.desc[2];
    __asm__ volatile("dmb sy" ::: "memory");
    if(head >= npu_glink.len || tail >= npu_glink.len) {
        printf("q8b npu: RX snapshot invalid head=%u tail=%u len=%u\n", head, tail, npu_glink.len);
        return;
    }
    uint32_t avail = head >= tail ? head - tail : npu_glink.len - tail + head;
    uint32_t words = (avail < 128u ? avail : 128u) / 4u;
    printf("q8b npu: RX snapshot head=%u tail=%u available=%u captured=%u\n",
            head, tail, avail, words * 4u);
    for(uint32_t i = 0; i < words; i++) {
        uint8_t b[4];
        for(unsigned j = 0; j < 4; j++) b[j] = npu_glink.rx[(tail + i * 4u + j) % npu_glink.len];
        printf("q8b npu: RX word offset=%u data=%08x\n", i * 4u,
                (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    }
}

static uint32_t glink_ring_add(uint32_t p, uint32_t n, uint32_t len) {
    p += n;
    while(p >= len) p -= len;
    return p;
}

static uint32_t glink_ring_used(uint32_t head, uint32_t tail, uint32_t len) {
    return head >= tail ? head - tail : len - tail + head;
}

static void glink_ring_copy(uint8_t *ring, uint32_t pos, void *dst,
        uint32_t len) {
    uint32_t first = len;
    if(pos + first > npu_glink.len) first = npu_glink.len - pos;
    memcpy(dst, ring + pos, first);
    if(first != len) memcpy((uint8_t *)dst + first, ring, len - first);
}

static void glink_ring_write(uint8_t *ring, uint32_t pos, const void *src,
        uint32_t len) {
    uint32_t first = len;
    if(pos + first > npu_glink.len) first = npu_glink.len - pos;
    memcpy(ring + pos, src, first);
    if(first != len) memcpy(ring, (const uint8_t *)src + first, len - first);
}

static void glink_rx_advance(uint32_t tail) {
    /* SMEM is Normal-NC on Q8B. Complete FIFO reads before publishing space
     * that the DSP may immediately overwrite; volatile alone is not a DMA
     * ordering primitive. */
    __asm__ volatile("dmb sy" ::: "memory");
    npu_glink.desc[2] = tail;
}

static int glink_tx_frame(uint16_t cmd, uint16_t p1, uint32_t p2,
        const void *data, uint32_t data_len) {
    uint8_t hdr[8];
    uint32_t head, tail, total;

    if(!npu_glink.ready || data_len > NPU_GLINK_FIFOMAX)
        return -1;
    total = (8u + data_len + 7u) & ~7u;
    head = npu_glink.desc[1];
    tail = npu_glink.desc[0];
    __asm__ volatile("dmb sy" ::: "memory");
    if(npu_glink.len - glink_ring_used(head, tail, npu_glink.len) <
            total + NPU_GLINK_FIFO_RESERVE)
        return -2;
    hdr[0] = (uint8_t)cmd; hdr[1] = (uint8_t)(cmd >> 8);
    hdr[2] = (uint8_t)p1; hdr[3] = (uint8_t)(p1 >> 8);
    hdr[4] = (uint8_t)p2; hdr[5] = (uint8_t)(p2 >> 8);
    hdr[6] = (uint8_t)(p2 >> 16); hdr[7] = (uint8_t)(p2 >> 24);
    glink_ring_write(npu_glink.tx, head, hdr, sizeof(hdr));
    head = glink_ring_add(head, sizeof(hdr), npu_glink.len);
    if(data_len) {
        glink_ring_write(npu_glink.tx, head, data, data_len);
        head = glink_ring_add(head, data_len, npu_glink.len);
    }
    while(head & 7u) {
        npu_glink.tx[head] = 0;
        head = glink_ring_add(head, 1, npu_glink.len);
    }
    __asm__ volatile("dmb sy" ::: "memory");
    npu_glink.desc[1] = head;
    __asm__ volatile("dsb sy" ::: "memory");
    if(cmd == 2 || cmd == 4 || cmd == 5 || cmd == 7 || cmd == 8 || cmd == 14)
        npu_debug("q8b npu: glink tx cmd=%u p1=%u p2=%u head=%u tail=%u\n",
                cmd, p1, p2, head, tail);
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060000u;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int glink_tx_data_frame(uint16_t cmd, uint16_t cid, uint32_t iid,
        const void *data, uint32_t size, uint32_t left) {
    uint8_t hdr[16];
    uint32_t head, tail, total;

    if(!npu_glink.ready || size > NPU_GLINK_FIFOMAX)
        return -1;
    total = (16u + size + 7u) & ~7u;
    head = npu_glink.desc[1];
    tail = npu_glink.desc[0];
    __asm__ volatile("dmb sy" ::: "memory");
    if(npu_glink.len - glink_ring_used(head, tail, npu_glink.len) <
            total + NPU_GLINK_FIFO_RESERVE)
        return -2;
    hdr[0] = (uint8_t)cmd; hdr[1] = (uint8_t)(cmd >> 8);
    hdr[2] = (uint8_t)cid; hdr[3] = (uint8_t)(cid >> 8);
    hdr[4] = (uint8_t)iid; hdr[5] = (uint8_t)(iid >> 8);
    hdr[6] = (uint8_t)(iid >> 16); hdr[7] = (uint8_t)(iid >> 24);
    hdr[8] = (uint8_t)size; hdr[9] = (uint8_t)(size >> 8);
    hdr[10] = (uint8_t)(size >> 16); hdr[11] = (uint8_t)(size >> 24);
    hdr[12] = (uint8_t)left; hdr[13] = (uint8_t)(left >> 8);
    hdr[14] = (uint8_t)(left >> 16); hdr[15] = (uint8_t)(left >> 24);
    glink_ring_write(npu_glink.tx, head, hdr, sizeof(hdr));
    head = glink_ring_add(head, sizeof(hdr), npu_glink.len);
    glink_ring_write(npu_glink.tx, head, data, size);
    head = glink_ring_add(head, size, npu_glink.len);
    while(head & 7u) {
        npu_glink.tx[head] = 0;
        head = glink_ring_add(head, 1, npu_glink.len);
    }
    __asm__ volatile("dmb sy" ::: "memory");
    npu_glink.desc[1] = head;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060000u;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static void glink_advertise_intent(void) {
    uint8_t data[8];
    uint32_t size = NPU_FRPC_LOCAL_INTENT_SIZE;

    data[0] = (uint8_t)size; data[1] = (uint8_t)(size >> 8);
    data[2] = (uint8_t)(size >> 16); data[3] = (uint8_t)(size >> 24);
    for(uint32_t iid = 1; iid <= NPU_FRPC_LOCAL_INTENT_COUNT; iid++) {
        data[4] = (uint8_t)iid; data[5] = (uint8_t)(iid >> 8);
        data[6] = (uint8_t)(iid >> 16); data[7] = (uint8_t)(iid >> 24);
        (void)glink_tx_frame(5, npu_glink.cid, 1, data, sizeof(data));
    }
    npu_glink.local_iid_count = NPU_FRPC_LOCAL_INTENT_COUNT;
}

/* fastrpc has an rpmsg callback, so the rpmsg core creates an endpoint after
 * the DSP announces the channel.  qcom_glink_create_remote() then allocates
 * an AP local CID and sends a matching OPEN before announcing intents. */
static void glink_send_local_open(void) {
    static const char name[] = NPU_FRPC_CHANNEL;
    npu_glink.cid = 1;
    npu_glink.open_sent = 1;
    (void)glink_tx_frame(2, npu_glink.cid, sizeof(name), name, sizeof(name));
}

/* Ask the remote endpoint to allocate an RX intent for data that the AP is
 * about to send.  qcom_glink_native does this lazily from __qcom_glink_send()
 * when no remote intent is available; doing it as soon as the FastRPC channel
 * opens is necessary because this restricted transport has no rpmsg endpoint
 * worker to issue the request later. */
static void glink_request_remote_intent(uint32_t size) {
    if(size < NPU_FRPC_REMOTE_REQ_SIZE)
        size = NPU_FRPC_REMOTE_REQ_SIZE;
    if(size > NPU_FRPC_INTENT_SIZE)
        size = NPU_FRPC_INTENT_SIZE;
    /* A newly requested intent supersedes the small bootstrap intent.  Do
     * not let glink_send_data consume the old iid while waiting for the DSP's
     * larger INTENT response. */
    npu_glink.remote_valid = 0;
    npu_glink.remote_wait_done = 0;
    npu_glink.remote_isize = 0;
    npu_glink.remote_iid = 0;
    (void)glink_tx_frame(7, npu_glink.cid, size, 0, 0);
}

/* Limited QRTR endpoint for the NSP0 sysmon/SSCTL shutdown handshake. */
static void glink_qrtr_observe(const uint8_t *data, uint32_t size) {
    struct q8b_qrtr_packet p;
    if(q8b_qrtr_decode(data, size, &p) != 0) {
        printf("q8b npu: QRTR malformed bytes=%u\n", size);
        return;
    }
    npu_glink.qrtr.packets++;
    if(npu_glink.qrtr.packets <= 32)
        npu_debug("q8b npu: QRTR type=%u src=%u/%x dst=%u/%x size=%u confirm=%u\n",
                p.type, p.src_node, p.src_port, p.dst_node, p.dst_port,
                p.size, p.confirm);
    if(p.type == 4 && p.size >= 20 && q8b_qrtr_le32(p.data) == 4) {
        npu_debug("q8b npu: QRTR service=%u instance=%x node=%u port=%x\n",
                q8b_qrtr_le32(p.data + 4), q8b_qrtr_le32(p.data + 8),
                q8b_qrtr_le32(p.data + 12), q8b_qrtr_le32(p.data + 16));
        if(q8b_qrtr_le32(p.data + 4) == 43 &&
                q8b_qrtr_le32(p.data + 8) == 0x1702 &&
                q8b_qrtr_le32(p.data + 12) == p.src_node) {
            npu_glink.qrtr.ssctl_node = p.src_node;
            npu_glink.qrtr.ssctl_port = q8b_qrtr_le32(p.data + 16);
        }
    }
    if(npu_glink.qrtr.shutdown_pending &&
            q8b_ssctl_shutdown_result(&p, npu_glink.qrtr.ssctl_node,
                npu_glink.qrtr.ssctl_port, npu_glink.qrtr.ssctl_txn,
                &npu_glink.qrtr.shutdown_result)) {
        npu_glink.qrtr.shutdown_response = 1;
        npu_debug("q8b npu: SSCTL shutdown response txn=%u result=%d\n",
                npu_glink.qrtr.ssctl_txn, npu_glink.qrtr.shutdown_result);
    }
}

static void glink_qrtr_hello(void) {
    if(!npu_glink.version_ack || !npu_glink.qrtr.open_ack ||
            !npu_glink.qrtr.valid || npu_glink.qrtr.hello_sent ||
            npu_glink.qrtr.isize < 52) return;
    /* v1 header + zeroed 20-byte qrtr_ctrl_pkt, as Linux ns.c say_hello(). */
    uint32_t packet[13] = {1, 2, 1, 0xfffffffeu, 0, 20,
        0xffffffffu, 0xfffffffeu, 2, 0, 0, 0, 0};
    if(glink_tx_data_frame(9, npu_glink.qrtr.cid, npu_glink.qrtr.iid,
                packet, sizeof(packet), 0) == 0) {
        npu_glink.qrtr.valid = 0;
        npu_glink.qrtr.wait_done = 1;
        npu_glink.qrtr.hello_sent = 1;
        npu_debug("q8b npu: QRTR discovery HELLO sent\n");
    }
}

static int glink_handle_rx_data(uint32_t tail, uint32_t avail,
        uint16_t cmd, uint16_t cid, uint32_t iid) {
    uint8_t h[8];
    uint32_t chunk, left, frame;

    if(avail < 16) return 0;
    glink_ring_copy(npu_glink.rx, glink_ring_add(tail, 8, npu_glink.len),
            h, sizeof(h));
    chunk = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
            ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    left = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
            ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    frame = 16u + chunk;
    if(npu_glink.qrtr.remote_cid && cid == npu_glink.qrtr.remote_cid) {
        if(chunk > sizeof(npu_glink.qrtr.rx) - npu_glink.qrtr.rx_len ||
                avail < frame) return -1;
        glink_ring_copy(npu_glink.rx, glink_ring_add(tail, 16, npu_glink.len),
                npu_glink.qrtr.rx + npu_glink.qrtr.rx_len, chunk);
        npu_glink.qrtr.rx_len += chunk;
        glink_rx_advance(glink_ring_add(tail, (frame + 7u) & ~7u, npu_glink.len));
        if(!left) {
            glink_qrtr_observe(npu_glink.qrtr.rx, npu_glink.qrtr.rx_len);
            npu_glink.qrtr.rx_len = 0;
            if(iid >= 1 && iid <= 4)
                (void)glink_tx_frame(14, npu_glink.qrtr.cid, iid, 0, 0);
        }
        return 1;
    }
    if(chunk > NPU_FRPC_RX_MAX - npu_glink.rx_len || avail < frame)
        return -1;
    glink_ring_copy(npu_glink.rx, glink_ring_add(tail, 16, npu_glink.len),
            npu_glink.rx_buf + npu_glink.rx_len, chunk);
    npu_glink.rx_len += chunk;
    glink_rx_advance(glink_ring_add(tail, (frame + 7u) & ~7u,
            npu_glink.len));
    if(left == 0) {
        if(cid == npu_glink.remote_cid && npu_glink.rx_len >= 12) {
            uint64_t ctx;
            int32_t rv;
            int matched = 0;
            memcpy(&ctx, npu_glink.rx_buf, sizeof(ctx));
            memcpy(&rv, npu_glink.rx_buf + 8, sizeof(rv));
            unsigned slot = npu_glink.response_count++ % 16u;
            npu_glink.responses[slot].ctx = ctx;
            npu_glink.responses[slot].rv = rv;
            npu_glink.responses[slot].tick = npu_frpc_ticks();
            for (unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++) {
                npu_frpc_pending_t *p = &npu_glink.pending[i];
                if (p->ctx && p->ctx == ctx && !p->done) {
                    __asm__ volatile("dmb sy" ::: "memory");
                    /* The owner may have freed/unmapped its CPU buffer or
                     * exited. Publish to owned DMA only in validated POLL. */
                    p->retval = rv;
                    p->done = 1;
                    matched = 1;
                    break;
                }
            }
            if(!matched)
                printf("q8b npu: unmatched FastRPC response ctx=%llx rv=%d bytes=%u\n",
                        (unsigned long long)ctx, rv, npu_glink.rx_len);
        }
        if(cid == npu_glink.remote_cid && iid >= 1 &&
                iid <= npu_glink.local_iid_count)
            (void)glink_tx_frame(14, npu_glink.cid, iid, 0, 0);
        npu_glink.rx_len = 0;
    }
    (void)cmd;
    return 1;
}

static int glink_poll(void) {
    int handled = 0;
    (void)npu_ipcc_poll();
    for(unsigned pass = 0; pass < 64; pass++) {
        uint32_t head = npu_glink.desc[3], tail = npu_glink.desc[2];
        /* Pair the DSP's published head with subsequent reads of FIFO data.
         * Linux uses memcpy_fromio on its IO mapping; our Normal-NC mapping
         * needs explicit ordering around ordinary memcpy. */
        __asm__ volatile("dmb sy" ::: "memory");
        uint32_t avail = glink_ring_used(head, tail, npu_glink.len);
        uint8_t h[8];
        uint16_t cmd, p1;
        uint32_t p2, frame = 8;
        if(avail < sizeof(h)) break;
        glink_ring_copy(npu_glink.rx, tail, h, sizeof(h));
        cmd = (uint16_t)h[0] | ((uint16_t)h[1] << 8);
        p1 = (uint16_t)h[2] | ((uint16_t)h[3] << 8);
        p2 = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
             ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        handled++;
        if(cmd == 9 || cmd == 12) {
            if(glink_handle_rx_data(tail, avail, cmd, p1, p2) < 0) break;
            continue;
        }
        if(cmd == 0 || cmd == 1) {
            if(cmd == 0) (void)glink_tx_frame(1, NPU_GLINK_VERSION,
                    NPU_GLINK_FEATURES, 0, 0);
            npu_glink.version_ack = 1;
            /* The DSP accepts channel traffic only after GLINK version
             * negotiation.  Defer RX_INTENT_REQ until this point; sending
             * it from OPEN is silently ignored by the production firmware. */
            if(npu_glink.open_ack && !npu_glink.remote_valid)
                glink_request_remote_intent(NPU_FRPC_REMOTE_REQ_SIZE);
        } else if(cmd == 2) {
            uint32_t name_len = p2 & 0xffffu;
            char name[64];
            if(name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            if(avail < 8u + name_len) break;
            glink_ring_copy(npu_glink.rx, glink_ring_add(tail, 8,
                    npu_glink.len), name, name_len);
            name[name_len] = 0;
            npu_debug("q8b npu: glink open name=%s cid=%u\n", name, p1);
            (void)glink_tx_frame(4, p1, 0, 0, 0);
            frame += (name_len + 7u) & ~7u;
            if(strcmp(name, NPU_FRPC_CHANNEL) == 0) {
                npu_glink.remote_cid = p1;
                npu_glink.local_iid = 1;
                npu_glink.remote_iid = 0;
                npu_glink.open_ack = 0;
                glink_send_local_open();
            } else if(strcmp(name, "IPCRTR") == 0) {
                static const char qrtr_name[] = "IPCRTR";
                npu_glink.qrtr.cid = 2;
                npu_glink.qrtr.remote_cid = p1;
                (void)glink_tx_frame(2, 2, sizeof(qrtr_name), qrtr_name, sizeof(qrtr_name));
            }
        } else if(cmd == 4) {
            /* OPEN_ACK addresses the AP local CID allocated above. */
            npu_debug("q8b npu: glink rx open_ack cid=%u expected=%u\n",
                    p1, npu_glink.cid);
            if(npu_glink.open_sent && p1 == npu_glink.cid) {
                npu_glink.open_ack = 1;
                npu_debug("q8b npu: glink open acknowledged cid=%u remote=%u\n",
                        npu_glink.cid, npu_glink.remote_cid);
                glink_advertise_intent();
                if(npu_glink.version_ack && !npu_glink.remote_valid)
                    glink_request_remote_intent(NPU_FRPC_REMOTE_REQ_SIZE);
            } else if(npu_glink.qrtr.cid && p1 == npu_glink.qrtr.cid) {
                npu_glink.qrtr.open_ack = 1;
                for(uint32_t iid = 1; iid <= 4; iid++) {
                    uint32_t intent[2] = {sizeof(npu_glink.qrtr.rx), iid};
                    (void)glink_tx_frame(5, p1, 1, intent, sizeof(intent));
                }
                if(!npu_glink.qrtr.valid)
                    (void)glink_tx_frame(7, p1, 64, 0, 0);
            }
        } else if(cmd == 5) {
            uint32_t count = p2;
            uint32_t best_size = 0, best_iid = 0;
            if(count > 32) {
                printf("q8b npu: glink intent count too large=%u\n", count);
                break;
            }
            frame += count * 8u;
            if(avail < frame) break;
            if((p1 == npu_glink.remote_cid || p1 == npu_glink.qrtr.remote_cid) && count) {
                uint8_t d[8];
                for (uint32_t k = 0; k < count; k++) {
                    glink_ring_copy(npu_glink.rx, glink_ring_add(tail,
                            8u + k * 8u, npu_glink.len), d, sizeof(d));
                    uint32_t isize = (uint32_t)d[0] |
                        ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
                        ((uint32_t)d[3] << 24);
                    uint32_t iid = (uint32_t)d[4] |
                        ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) |
                        ((uint32_t)d[7] << 24);
                    if (isize > best_size) {
                        best_size = isize;
                        best_iid = iid;
                    }
                }
                if (best_size) {
                    if(p1 == npu_glink.qrtr.remote_cid) {
                        npu_glink.qrtr.isize = best_size;
                        npu_glink.qrtr.iid = best_iid;
                        npu_glink.qrtr.valid = 1;
                        npu_glink.qrtr.wait_done = 0;
                    } else {
                        npu_glink.remote_isize = best_size;
                        npu_glink.remote_iid = best_iid;
                        npu_glink.remote_valid = 1;
                        npu_glink.remote_wait_done = 0;
                        npu_debug("q8b npu: glink intent cid=%u size=%u iid=%u\n",
                                p1, npu_glink.remote_isize,
                                npu_glink.remote_iid);
                    }
                }
            }
        } else if(cmd == 8) {
            npu_debug("q8b npu: glink rx_intent_ack cid=%u count=%u\n", p1, p2);
        } else if(cmd == 6 || cmd == 14) {
            /* The remote endpoint reports completion for an intent that AP
             * consumed with TX_DATA.  Mainline qcom_glink removes a normal
             * intent on RX_DONE and marks a reusable one available on
             * RX_DONE_W_REUSE. */
            if(p1 == npu_glink.qrtr.remote_cid && npu_glink.qrtr.wait_done &&
                    p2 == npu_glink.qrtr.iid) {
                npu_glink.qrtr.valid = cmd == 14;
                npu_glink.qrtr.wait_done = 0;
            } else if (p1 == npu_glink.remote_cid &&
                    npu_glink.remote_wait_done &&
                    p2 == npu_glink.remote_iid) {
                if (cmd == 14) {
                    npu_glink.remote_valid = 1;
                    npu_glink.remote_wait_done = 0;
                } else {
                    npu_glink.remote_valid = 0;
                    npu_glink.remote_wait_done = 0;
                    npu_glink.remote_iid = 0;
                    npu_glink.remote_isize = 0;
                }
            }
        } else if(cmd == 7) {
            npu_debug("q8b npu: glink rx_intent_req cid=%u size=%u\n", p1, p2);
            /* The remote may request a receive intent instead of advertising
             * one proactively.  Linux qcom_glink answers the request and
             * then advertises its local buffer; without the second message
             * FastRPC's first TX_DATA has no remote intent to consume it. */
            if(npu_glink.qrtr.remote_cid && p1 == npu_glink.qrtr.remote_cid) {
                int fits = p2 <= sizeof(npu_glink.qrtr.rx);
                (void)glink_tx_frame(8, npu_glink.qrtr.cid, fits, 0, 0);
                if(fits) for(uint32_t iid = 1; iid <= 4; iid++) {
                    uint32_t intent[2] = {sizeof(npu_glink.qrtr.rx), iid};
                    (void)glink_tx_frame(5, npu_glink.qrtr.cid, 1, intent, sizeof(intent));
                }
            } else if(p1 == npu_glink.remote_cid && npu_glink.local_iid) {
                (void)glink_tx_frame(8, npu_glink.cid, 1, 0, 0);
                glink_advertise_intent();
            }
        } else if(cmd == 3) {
            (void)glink_tx_frame(11, p1, 0, 0, 0);
        } else {
            printf("q8b npu: glink rx unhandled cmd=%u p1=%u p2=%u\n",
                    cmd, p1, p2);
        }
        glink_rx_advance(glink_ring_add(tail, (frame + 7u) & ~7u,
                npu_glink.len));
    }
    glink_qrtr_hello();
    return handled;
}

static int glink_ssctl_shutdown(void) {
    if(!npu_glink.qrtr.open_ack) return -1;
    /* sysmon_stop() waits briefly for service discovery, then asks SSCTL
     * before q6v5's fallback SMP2P dance. NSP0 has no shutdown-ack IRQ, so
     * mainline accepts the successful QMI response itself. */
    int requested = 0;
    for(unsigned i = 0; i < 500; i++) {
        (void)glink_poll();
        if(npu_glink.qrtr.ssctl_port && npu_glink.qrtr.valid &&
                npu_glink.qrtr.isize >= 40) break;
        /* HELLO can consume a non-reusable bootstrap intent (RX_DONE).
         * Ask for a new buffer rather than assuming it remains available. */
        if(npu_glink.qrtr.ssctl_port && !requested &&
                !npu_glink.qrtr.wait_done &&
                (!npu_glink.qrtr.valid || npu_glink.qrtr.isize < 40)) {
            if(glink_tx_frame(7, npu_glink.qrtr.cid, 64, 0, 0) == 0)
                requested = 1;
        }
        npu_delay_ms(1);
    }
    if(!npu_glink.qrtr.ssctl_port || !npu_glink.qrtr.valid ||
            npu_glink.qrtr.isize < 40) return -1;
    uint8_t packet[40];
    npu_glink.qrtr.ssctl_txn++;
    npu_glink.qrtr.shutdown_response = 0;
    npu_glink.qrtr.shutdown_result = -1;
    q8b_ssctl_shutdown_request(packet, npu_glink.qrtr.ssctl_node,
            npu_glink.qrtr.ssctl_port, npu_glink.qrtr.ssctl_txn);
    if(glink_tx_data_frame(9, npu_glink.qrtr.cid, npu_glink.qrtr.iid,
                packet, sizeof(packet), 0) != 0) return -1;
    npu_glink.qrtr.valid = 0;
    npu_glink.qrtr.wait_done = 1;
    npu_glink.qrtr.shutdown_pending = 1;
    npu_debug("q8b npu: SSCTL shutdown requested node=%u port=%u txn=%u\n",
            npu_glink.qrtr.ssctl_node, npu_glink.qrtr.ssctl_port, npu_glink.qrtr.ssctl_txn);
    for(unsigned i = 0; i < 5000; i++) {
        (void)glink_poll();
        if(npu_glink.qrtr.shutdown_response) break;
        if(npu_smp2p_refresh_state() & Q8B_NPU_STATE_FATAL) break;
        npu_delay_ms(1);
    }
    npu_glink.qrtr.shutdown_pending = 0;
    if(!npu_glink.qrtr.shutdown_response || npu_glink.qrtr.shutdown_result) {
        printf("q8b npu: SSCTL shutdown failed; using SMP2P\n");
        return -1;
    }
    npu_debug("q8b npu: SSCTL shutdown acknowledged\n");
    return 0;
}

static int glink_send_data(const void *data, uint32_t size) {
    uint32_t off = 0, chunk;
    if(!npu_glink.cid || !size || size > NPU_FRPC_INTENT_SIZE)
        return -1;
    /* qcom_glink_send() requests an intent at least as large as the whole
     * message before emitting TX_DATA.  The bootstrap exchange may have
     * produced only a 1 KiB intent; negotiate a larger one for a real
     * FastRPC packet instead of sending an 8 KiB chunk into a 1 KiB buffer. */
    if(!npu_glink.remote_valid || npu_glink.remote_isize < size) {
        glink_request_remote_intent(size);
        for(unsigned tries = 0; tries < 5000; tries++) {
            (void)glink_poll();
            if(npu_glink.remote_valid && npu_glink.remote_isize >= size)
                break;
            npu_delay_ms(1);
        }
        if(!npu_glink.remote_valid || npu_glink.remote_isize < size)
            return -2;
    }
    while(off < size) {
        chunk = size - off;
        if(chunk > 8192) chunk = 8192;
        if(glink_tx_data_frame(off ? 12 : 9, npu_glink.cid,
                npu_glink.remote_iid, (const uint8_t *)data + off, chunk,
                size - off - chunk) != 0)
            return -1;
        off += chunk;
    }
    /* Do not reuse a non-reusable intent until the DSP acknowledges it.  If
     * it is reusable, RX_DONE_W_REUSE will set remote_valid again while the
     * response is being polled. */
    npu_glink.remote_valid = 0;
    npu_glink.remote_wait_done = 1;
    return 0;
}

static void glink_ping(void) {
    /* A remoteproc GLINK edge is registered once per hardware boot. A
     * SESSION retry may poll negotiation, but must not reset live FIFOs. */
    if(npu_glink.ready) {
        for(unsigned tries = 0; tries < 100; tries++) {
            (void)glink_poll();
            if(npu_glink.version_ack && npu_glink.open_ack &&
                    npu_glink.remote_valid)
                break;
            npu_delay_ms(20);
        }
        return;
    }
    uint8_t *s = (uint8_t *)Q8B_SMEM_WIN;
    uint8_t *ph = s + 0x1af000;
    uint32_t fU = *(uint32_t *)(ph + 12);
    const uint8_t *i478 = smem_find_private_item(ph, fU, 478);
    const uint8_t *i479 = smem_find_private_item(ph, fU, 479);
    const uint8_t *i480 = smem_find_private_item(ph, fU, 480);
    npu_debug("q8b npu: glink items %d %d %d\n", i478 != 0, i479 != 0, i480 != 0);
    if(!i478 || !i479 || !i480) return;
    memset(&npu_glink, 0, sizeof(npu_glink));
    npu_glink.desc = (volatile uint32_t *)(i478 + 16);
    npu_glink.tx = (uint8_t *)(i479 + 16);
    npu_glink.rx = (uint8_t *)(i480 + 16);
    npu_glink.len = 16384;
    npu_glink.ready = 1;
    npu_debug("q8b npu: glink desc %x %x %x %x\n", npu_glink.desc[0],
            npu_glink.desc[1], npu_glink.desc[2], npu_glink.desc[3]);
    /* qcom_glink_smem_register initializes only AP-owned TX head/RX tail.
     * The DSP may have published VERSION before AP registers this edge;
     * clearing its RX head would discard that frame. Fresh PAS readiness
     * is checked by both callers before this first initialization. */
    npu_glink.desc[1] = 0;
    npu_glink.desc[2] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    npu_debug("q8b npu: GLINK edge initialized once; DSP tail=%x head=%x preserved\n",
            npu_glink.desc[0], npu_glink.desc[3]);
    (void)glink_tx_frame(0, NPU_GLINK_VERSION, NPU_GLINK_FEATURES, 0, 0);
    npu_glink.version_sent = 1;
    for(unsigned tries = 0; tries < 100; tries++) {
        (void)glink_poll();
        if(npu_glink.version_ack && npu_glink.open_ack &&
                npu_glink.remote_valid)
            break;
        npu_delay_ms(20);
    }
    npu_debug("q8b npu: glink %s cid=%u iid=%u\n",
            npu_glink.version_ack ? "version acknowledged" : "no response",
            npu_glink.cid, npu_glink.remote_iid);
}

/* The compact PAS_BOOT path waits for the DSP-owned RX FIFO before probing
 * GLINK.  Keep the five-step userspace ABI at the same handover point;
 * otherwise PAS_AUTH can race the firmware's SMEM allocator and observe only
 * host items 478/479, causing the first FastRPC session to be declined. */
static void glink_wait_for_remote_items(void) {
    uint8_t *ph = (uint8_t *)Q8B_SMEM_WIN + 0x1af000;
    int seen = 0;

    for(int try = 0; try < 50 && !seen; try++) {
        uint32_t fU = *(volatile uint32_t *)(ph + 12);
        const uint8_t *it = smem_find_private_item(ph, fU, 480);
        if(it) {
            seen = 1;
            break;
        }
        *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
        __asm__ volatile("dsb sy" ::: "memory");
        npu_delay_ms(100);
    }
    npu_debug("q8b npu: glink remote item 480 seen=%d\n", seen);
}

/* Re-arm before AUTH, as q6v5_prepare() reinitializes start_done. The
 * previous firmware may leave READY/HANDOVER high even without STOP_ACK.
 * Consume a new SMP2P delivery before allowing a fresh GLINK edge. */
static int npu_wait_for_ready(void) {
    for (unsigned attempt = 0; attempt < 50; attempt++) {
        uint32_t state = npu_smp2p_refresh_state();
        if (state & Q8B_NPU_STATE_FATAL) break;
        if (q8b_npu_state_ready(state) && npu_boot_smp2p_notified) {
            npu_debug("q8b npu: DSP ready after new SMP2P notification\n");
            npu_watchdog_snapshot("ready");
            return 0;
        }
        *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
        __asm__ volatile("dsb sy" ::: "memory");
        npu_delay_ms(100);
    }
    npu_recovery_required = 1;
    printf("q8b npu: new DSP readiness not observed notify=%d pending=%x; preserving GLINK, board reset required\n",
            npu_boot_smp2p_notified, npu_ipcc_recv(0));
    return Q8B_NPU_PAS_FAILED;
}

static uint64_t stage_and_boot(void *fw, uint32_t fw_size, uint64_t fw_pa,
        uint32_t capacity) {
    const npu_elf32_hdr *eh = (const npu_elf32_hdr *)fw;
    const npu_elf32_phdr *ph;
    uint64_t min_addr = ~0ull, max_addr = 0, meta_len;
    uint32_t i, hash = 0;
    int ret;
    uint64_t result = 0;
    uint8_t *dst;

    if(fw_size < sizeof(*eh) || eh->e_ident[0] != 0x7f ||
            eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
            eh->e_ident[3] != 'F' || eh->e_ident[4] != 1)
        return Q8B_NPU_PAS_INVALID;
    if(eh->e_phnum < 2 || eh->e_phnum > 32 ||
            eh->e_phentsize != sizeof(*ph) ||
            (uint64_t)eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(*ph) > fw_size)
        return Q8B_NPU_PAS_INVALID;
    ph = (const npu_elf32_phdr *)((const uint8_t *)fw + eh->e_phoff);

    for(i = 0; i < eh->e_phnum; i++) {
        uint64_t end;
        if(!q8b_mdt_loadable(&ph[i])) {
            if(!hash && (ph[i].p_flags & NPU_MDT_TYPE_MASK) ==
                    NPU_MDT_TYPE_HASH)
                hash = i;
            continue;
        }
        end = (uint64_t)ph[i].p_paddr + ph[i].p_memsz;
        if(ph[i].p_paddr < min_addr) min_addr = ph[i].p_paddr;
        if(end > max_addr) max_addr = end;
    }
    if(!hash || min_addr == ~0ull || max_addr <= min_addr)
        return Q8B_NPU_PAS_INVALID;
    max_addr = (max_addr + 0xfff) & ~0xfffull;
    if(max_addr - min_addr != Q8B_NSP0_SPAN)
        return Q8B_NPU_PAS_INVALID;
    if(q8b_mdt_validate_loads(fw_size, ph, eh->e_phnum, min_addr,
                Q8B_NSP0_SPAN) != 0)
        return Q8B_NPU_PAS_INVALID;

    /* metadata = ELF header block + hash segment, no padding */
    meta_len = (uint64_t)ph[0].p_filesz + ph[hash].p_filesz;
    if(meta_len > NPU_META_SIZE || ph[0].p_filesz > fw_size ||
            (uint64_t)ph[hash].p_offset + ph[hash].p_filesz > fw_size)
        return Q8B_NPU_PAS_INVALID;
    /* A new attempt invalidates the prior stopped-RAM snapshot, including
     * a subsequent authentication/prepare failure. */
    npu_dump_available = 0;
    /* The metadata lives in the caller's DMA buffer, after the image, so it
     * is mapped in the syscall's address space (kernel-private dma_alloc
     * pages are not). */
    {
        uint32_t off = (fw_size + 15u) & ~15u;
        if((uint64_t)off + meta_len > capacity)
            return Q8B_NPU_PAS_INVALID;
        meta_va = (uint8_t *)fw + off;
        meta_pa = fw_pa + off;
    }
    for(i = 0; i < ph[0].p_filesz; i++)
        ((uint8_t *)meta_va)[i] = ((const uint8_t *)fw)[i];
    for(i = 0; i < ph[hash].p_filesz; i++)
        ((uint8_t *)meta_va)[ph[0].p_filesz + i] =
                ((const uint8_t *)fw)[ph[hash].p_offset + i];
    __asm__ volatile("dsb sy" ::: "memory");

    if(npu_watchdog_prepare() != 0) return Q8B_NPU_PAS_FAILED;
    if(nsp_power_vote() != 0) {
        npu_recovery_required = 1;
        return Q8B_NPU_PAS_FAILED;
    }
    npu_ipcc_enable(0); /* GLINK_QMP */
    npu_ipcc_enable(2); /* SMP2P */
    smem_prepare();
    /* Tell the DSP the host SMP2P item is ready before releasing reset. */
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
    __asm__ volatile("dsb sy" ::: "memory");
    npu_debug("q8b npu: SMP2P pre-boot kick\n");

    npu_debug("q8b npu: fw min=%llx max=%llx span=%llx meta=%llu hash=%u\n",
            (unsigned long long)min_addr, (unsigned long long)max_addr,
            (unsigned long long)(max_addr - min_addr),
            (unsigned long long)meta_len, hash);

    ret = scm_call(0x201, 0x82, Q8B_NSP0_PAS_ID, meta_pa,
            (uint32_t)meta_len, 0, &result);
    npu_debug("q8b npu: init ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == Q8B_GPU_PAS_BUSY ? Q8B_NPU_PAS_BUSY :
            Q8B_NPU_PAS_FAILED;
    if(result) return Q8B_NPU_PAS_FAILED;

    ret = scm_call(0x202, 3, Q8B_NSP0_PAS_ID, Q8B_NSP0_CARVEOUT,
            Q8B_NSP0_SPAN, 0, &result);
    npu_debug("q8b npu: mem_setup ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == Q8B_GPU_PAS_BUSY ? Q8B_NPU_PAS_BUSY :
            Q8B_NPU_PAS_FAILED;
    if(result) return Q8B_NPU_PAS_FAILED;

    {
        /* A syscall runs on the caller's page tables, so the carveout must be
         * present in both the kernel and the current process address space. */
        proc_t *p = proc_get_proc(get_current_proc());
        page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;

        if(p->info.pid != win_pid) {
            if(map_pages_size(_kernel_info.kernel_vm, Q8B_NSP_WIN,
                        Q8B_NSP0_CARVEOUT, Q8B_NSP0_SPAN,
                        AP_RW_D, PTE_ATTR_SYS_DMA) != 0)
                return Q8B_NPU_PAS_FAILED;
            if(pvm != 0 && pvm != _kernel_info.kernel_vm &&
                    map_pages_size(pvm, Q8B_NSP_WIN,
                        Q8B_NSP0_CARVEOUT, Q8B_NSP0_SPAN,
                        AP_RW_D, PTE_ATTR_SYS_DMA) != 0)
                return Q8B_NPU_PAS_FAILED;
            win_pid = p->info.pid;
        }
        flush_tlb();
        __asm__ volatile("dsb sy" ::: "memory");
    }
    dst = (uint8_t *)Q8B_NSP_WIN;
    if(q8b_mdt_stage(dst, Q8B_NSP0_SPAN, fw, fw_size, ph,
                eh->e_phnum, min_addr) != 0)
        return Q8B_NPU_PAS_INVALID;
    __asm__ volatile("dsb sy" ::: "memory");
    npu_debug("q8b npu: image staged; PT_LOAD BSS zeroed, gaps preserved\n");

    npu_boot_notify_arm();
    ret = scm_call(0x205, 1, Q8B_NSP0_PAS_ID, 0, 0, 0, &result);
    npu_debug("q8b npu: auth_and_reset ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if(ret) return ret == Q8B_GPU_PAS_BUSY ? Q8B_NPU_PAS_BUSY :
            Q8B_NPU_PAS_FAILED;
    if(result) return Q8B_NPU_PAS_FAILED;
    booted = 1;
    npu_smp2p_state = Q8B_NPU_STATE_BOOTED;
    printf("q8b npu: NSP0 authenticated and out of reset\n");
    if(npu_wait_for_ready() != 0) return Q8B_NPU_PAS_FAILED;
    glink_ping();
    return 0;
}
#endif

static int owned_dma(ewokos_addr_t pa, ewokos_addr_t len, uint32_t max,
        uint32_t *capacity) {
    proc_t *p = proc_get_proc(get_current_proc());
    uint32_t cap = dma_size(0, p->info.pid, pa);

    if(capacity) *capacity = cap;
    return len && len <= max && !(pa & 0xfff) && cap >= len;
}

struct npu_fastrpc_msg {
    int32_t client_id;
    int32_t tid;
    uint64_t ctx;
    uint32_t handle;
    uint32_t sc;
    uint64_t addr;
    uint64_t size;
} __attribute__((packed));

static uint32_t npu_frpc_session;
/* Linux fastrpc_msg.ctx carries the protection-domain id in its low bits.
 * INIT_ATTACH starts in ROOT_PD (0); INIT_CREATE switches the client to
 * USER_PD (1), and that bit remains set for mappings, skel calls and the
 * eventual INIT_RELEASE. */
static uint32_t npu_frpc_pd;

/* FastRPC compute-cb@3 addresses are (SID << 32) | SMMU IOVA.  Keep the
 * ownership table in the kernel so userspace can only map its own DMA pages,
 * and so invoke can resolve the packet before placing its address on GLINK. */
#define NPU_FRPC_SID              3u
#define NPU_FRPC_MAP_MAX          64u
#define NPU_FRPC_MAX_RBUFS        32u
#define NPU_FRPC_IOVA_BASE        0x10000000ull
#define NPU_FRPC_IOVA_LIMIT       0xf0000000ull
typedef struct {
    uint64_t pa;
    uint64_t size;
    uint64_t iova;
    uint64_t dsp_vaddr;
    uint32_t session;
    uint32_t refs;
    int used;
} npu_frpc_map_t;

static npu_frpc_map_t npu_frpc_maps[NPU_FRPC_MAP_MAX];
static uint64_t npu_frpc_next_iova = NPU_FRPC_IOVA_BASE;

/* Only after successful SMMU teardown. Each distinct mapping holds one
 * allocator pin, independently of its userspace registration refcount. */
static int npu_frpc_release_maps(void) {
    int rc = 0;
    for (unsigned i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (!m->used) continue;
        if (dma_unpin(0, m->pa) != 0) {
            printf("q8b npu: DMA pin release failed pa=%llx; retaining record\n",
                    (unsigned long long)m->pa);
            npu_recovery_required = 1;
            rc = -1;
            continue;
        }
        memset(m, 0, sizeof(*m));
    }
    if (!rc) npu_frpc_next_iova = NPU_FRPC_IOVA_BASE;
    return rc;
}

/* A software allocator is not enough: do not expose an address until a context bank,
 * SID 0x3183 routing and the stage-1 page table are programmed together. */
static uint64_t npu_frpc_map_lookup(uint32_t session, uint64_t pa,
        uint32_t size) {
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (m->used && m->session == session && m->pa == pa &&
                m->size >= size)
            return ((uint64_t)NPU_FRPC_SID << 32) | m->iova;
    }
    return 0;
}

static int npu_frpc_map(uint64_t arg, uint32_t size, uint64_t *devaddr) {
    uint32_t session = (uint32_t)(arg >> 32);
    uint64_t pa = (arg & 0xffffffffull) << 12;
    uint64_t aligned_size;
    uint64_t iova;

    if (npu_recovery_required || !q8b_npu_smmu_ready() ||
            session != npu_frpc_session || !size ||
            (pa & 0xfffull))
        return Q8B_NPU_PAS_UNSUPPORTED;
    aligned_size = ((uint64_t)size + 0xfffull) & ~0xfffull;
    /* The SMMU always maps whole pages.  Check ownership against the same
     * rounded extent that will be published, otherwise a short DMA
     * allocation could accidentally expose the following allocation. */
    if (aligned_size > UINT32_MAX ||
            !owned_dma((ewokos_addr_t)pa, (ewokos_addr_t)aligned_size,
                NPU_MAX_DMA, 0))
        return Q8B_NPU_PAS_INVALID;
    *devaddr = npu_frpc_map_lookup(session, pa, size);
    if (*devaddr) {
        for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
            npu_frpc_map_t *m = &npu_frpc_maps[i];
            if (m->used && m->session == session && m->pa == pa &&
                    m->size >= size) {
                if (m->refs == UINT32_MAX)
                    return Q8B_NPU_PAS_BUSY;
                m->refs++;
                break;
            }
        }
        return 0;
    }
    iova = (npu_frpc_next_iova + 0xfffull) & ~0xfffull;
    if (iova < NPU_FRPC_IOVA_BASE ||
            aligned_size > NPU_FRPC_IOVA_LIMIT - iova)
        return Q8B_NPU_PAS_FAILED;
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (!m->used) {
            proc_t *owner = proc_get_proc(get_current_proc());
            if (dma_pin(0, owner->info.pid, pa, (uint32_t)aligned_size) != 0)
                return Q8B_NPU_PAS_UNSUPPORTED;
            memset(m, 0, sizeof(*m));
            m->used = 1;
            m->pa = pa;
            m->size = aligned_size;
            m->iova = iova;
            m->session = session;
            m->refs = 1;
            if (q8b_npu_smmu_map(pa, iova, (uint32_t)aligned_size) != 0) {
                /* A failed publish may include a failed TLB invalidation.
                 * Keep the pin/record until full teardown, even though no
                 * device address was returned to the caller. */
                npu_recovery_required = 1;
                printf("q8b npu: SMMU map failed; retaining DMA pin pa=%llx\n",
                        (unsigned long long)pa);
                return Q8B_NPU_PAS_FAILED;
            }
            npu_frpc_next_iova = iova + aligned_size;
            *devaddr = ((uint64_t)NPU_FRPC_SID << 32) | iova;
            return 0;
        }
    }
    return Q8B_NPU_PAS_BUSY;
}

/* Retain every page referenced by an accepted request until POLL consumes
 * it.  Completion may have reached AP while DSP shared writes are pending;
 * the consumer's ordering barrier precedes dropping these registrations. */
static int npu_frpc_map_in_flight(const npu_frpc_map_t *m) {
    unsigned index = (unsigned)(m - npu_frpc_maps);
    for (unsigned j = 0; j < NPU_FRPC_PENDING_MAX; j++) {
        npu_frpc_pending_t *p = &npu_glink.pending[j];
        if (p->ctx && (p->maps & (1ull << index))) return 1;
    }
    return 0;
}

static int npu_frpc_unmap(uint64_t devaddr, uint32_t size) {
    uint32_t sid = (uint32_t)(devaddr >> 32);
    uint64_t iova = devaddr & 0xffffffffull;

    if (!q8b_npu_smmu_ready() || sid != NPU_FRPC_SID || !size ||
            !npu_frpc_session)
        return Q8B_NPU_PAS_UNSUPPORTED;
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (m->used && m->session == npu_frpc_session &&
                m->iova == iova && m->size >= size) {
            if (m->refs == 0)
                return Q8B_NPU_PAS_FAILED;
            if (m->refs == 1 && npu_frpc_map_in_flight(m)) return Q8B_NPU_PAS_BUSY;
            m->refs--;
            if (m->refs != 0)
                return 0;
            if (q8b_npu_smmu_unmap(iova, (uint32_t)m->size) != 0) {
                m->refs = 1;
                return Q8B_NPU_PAS_FAILED;
            }
            if (dma_unpin(0, m->pa) != 0) {
                m->refs = 1;
                npu_recovery_required = 1;
                return Q8B_NPU_PAS_FAILED;
            }
            m->used = 0;
            /* Invoke packets are short-lived.  Rewind the monotonic IOVA
             * cursor once the last registration is gone so repeated graph
             * execution cannot exhaust the 32-bit IOVA window merely because
             * old holes were left behind. */
            {
                int any = 0;
                for (uint32_t j = 0; j < NPU_FRPC_MAP_MAX; j++)
                    if (npu_frpc_maps[j].used) {
                        any = 1;
                        break;
                    }
                if (!any)
                    npu_frpc_next_iova = NPU_FRPC_IOVA_BASE;
            }
            return 0;
        }
    }
    return Q8B_NPU_PAS_INVALID;
}

static int npu_frpc_iova_owned(uint32_t session, uint64_t devaddr,
        uint64_t size) {
    uint32_t sid = (uint32_t)(devaddr >> 32);
    uint64_t iova = devaddr & 0xffffffffull;

    if (!size)
        return 0;
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (!m->used || m->session != session)
            continue;
        /* AP consolidated addresses carry the compute SID in the high
         * word.  A standard INIT_MMAP response is a DSP virtual address and
         * deliberately has no SID prefix; accept it only after this kernel
         * has recorded the response against the owning AP mapping. */
        if (sid == NPU_FRPC_SID && iova >= NPU_FRPC_IOVA_BASE &&
                iova < NPU_FRPC_IOVA_LIMIT &&
                size <= NPU_FRPC_IOVA_LIMIT - iova &&
                iova >= m->iova && iova + size <= m->iova + m->size)
            return 1;
        if (m->dsp_vaddr && devaddr >= m->dsp_vaddr &&
                size <= UINT64_MAX - m->dsp_vaddr &&
                devaddr + size <= m->dsp_vaddr + m->size)
            return 1;
    }
    return 0;
}

static void npu_frpc_record_dsp_mmap(uint32_t session, uint8_t *payload,
        uint64_t payload_iova, uint32_t payload_size) {
    struct q8b_frpc_map_reply reply;

    if (q8b_frpc_decode_map_reply(payload, payload_iova, payload_size,
                &reply) != 0 || (reply.ap_addr >> 32) != NPU_FRPC_SID)
        return;
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (!m->used || m->session != session ||
                (reply.ap_addr & 0xffffffffull) != m->iova ||
                reply.size > m->size)
            continue;
        /* A DSP address must not alias a different live AP mapping. */
        for (uint32_t j = 0; j < NPU_FRPC_MAP_MAX; j++) {
            if (j != i && npu_frpc_maps[j].used &&
                    npu_frpc_maps[j].dsp_vaddr == reply.dsp_vaddr)
                return;
        }
        m->dsp_vaddr = reply.dsp_vaddr;
        return;
    }
}

static void npu_frpc_release_dsp_mmap(uint32_t session, uint8_t *payload,
        uint64_t payload_iova, uint32_t payload_size, uint32_t handle,
        uint32_t sc) {
    uint32_t inbufs = (sc >> 16) & 0xffu;
    uint32_t outbufs = (sc >> 8) & 0xffu;
    uint32_t method = (sc >> 24) & 0x1fu;
    uint64_t dsp_vaddr, size;
    uint8_t *req;

    /* INIT_MUNMAP (5) uses frpc_munmap_req_msg, while the dma-buf form
     * INIT_MEM_UNMAP (11) uses frpc_mem_unmap_req_msg.  Both put the DSP
     * virtual address at offset 8 and the mapped byte length at offset 16;
     * accepting both here keeps the AP ownership entry alive until the DSP
     * has acknowledged either form. */
    if (handle != 1u || (method != 5u && method != 11u) ||
            inbufs != 1u || outbufs != 0u ||
            ((sc >> 4) & 0x0fu) || (sc & 0x0fu))
        return;
    req = q8b_frpc_inline_arg(payload, payload_iova, payload_size, 0, 24u);
    if (!req)
        return;
    memcpy(&dsp_vaddr, req + 8u, sizeof(dsp_vaddr));
    memcpy(&size, req + 16u, sizeof(size));
    if (!dsp_vaddr || !size)
        return;
    for (uint32_t i = 0; i < NPU_FRPC_MAP_MAX; i++) {
        npu_frpc_map_t *m = &npu_frpc_maps[i];
        if (m->used && m->session == session &&
                m->dsp_vaddr == dsp_vaddr && size <= m->size) {
            m->dsp_vaddr = 0;
            return;
        }
    }
}

static int npu_frpc_submit(ewokos_addr_t pa, uint32_t size, int listener) {
#ifdef Q8B_QEMU_TEST
    (void)pa;
    (void)size;
    (void)listener;
    return Q8B_NPU_PAS_UNSUPPORTED;
#else
    uint8_t *payload;
    uint32_t cap = 0, handle, sc;
    struct npu_fastrpc_msg msg;
    uint64_t ctx, msg_iova, map_mask = 0;
    uint32_t payload_session, inbufs, outbufs, inhandles, outhandles;
    uint32_t nbufs, nscalars, meta;
    uint64_t meta_size;
    npu_frpc_pending_t *pending = 0;

    /* Observe fatal/watchdog before consuming a response or submitting DMA. */
    if(npu_smp2p_refresh_state() & Q8B_NPU_STATE_FATAL)
        return Q8B_NPU_PAS_FAILED;
    if (npu_glink.desc) (void)glink_poll();
    for (unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++) {
        if (npu_glink.pending[i].ctx && npu_glink.pending[i].pa == pa)
            return Q8B_NPU_PAS_INVALID; /* never resubmit an accepted packet */
        if (!npu_glink.pending[i].ctx) pending = &npu_glink.pending[i];
    }
    if (!pending) return Q8B_NPU_PAS_BUSY;
    if(!booted || !q8b_npu_smmu_ready() || !npu_glink.ready || !npu_glink.cid)
        return Q8B_NPU_PAS_UNSUPPORTED;
    if (!npu_glink.remote_valid) return Q8B_NPU_PAS_BUSY;
    if(size < 16 || size > NPU_MAX_FW || !owned_dma(pa, size, NPU_MAX_FW,
            &cap) || cap < size)
        return Q8B_NPU_PAS_INVALID;
    payload = (uint8_t *)dma_v_addr(0, pa);
    if(!payload) return Q8B_NPU_PAS_INVALID;
    msg_iova = npu_frpc_map_lookup(npu_frpc_session, pa, size);
    if(!msg_iova) {
        printf("q8b npu: frpc invoke rejected (payload is not mapped)\n");
        return Q8B_NPU_PAS_UNSUPPORTED;
    }
    for (unsigned j = 0; j < NPU_FRPC_MAP_MAX; j++) {
        const npu_frpc_map_t *m = &npu_frpc_maps[j];
        if (m->used && m->pa == pa && m->size >= size)
            map_mask |= 1ull << j;
    }
    memcpy(&handle, payload + 8, sizeof(handle));
    memcpy(&sc, payload + 12, sizeof(sc));
    memcpy(&payload_session, payload, sizeof(payload_session));
    meta = q8b_frpc_metadata_offset(payload, size);
    if (!meta || (payload_session & ~(Q8B_NPU_FRPC_PACKET_PAGE | Q8B_NPU_FRPC_PACKET_HOST)) != npu_frpc_session)
        return Q8B_NPU_PAS_INVALID;
    /* Match REMOTE_SCALARS_* in Linux drivers/misc/fastrpc.c.  The high
     * byte carries the remote method; buffer counts are split between input
     * and output bytes, while the low nibbles describe handle arguments. */
    inbufs = (sc >> 16) & 0xffu;
    outbufs = (sc >> 8) & 0xffu;
    inhandles = (sc >> 4) & 0x0fu;
    outhandles = sc & 0x0fu;
    nbufs = inbufs + outbufs;
    nscalars = nbufs + inhandles + outhandles;
    /* This compact transport currently marshals buffer arguments only.  A
     * future handle-aware client can extend the metadata layout without
     * silently treating handles as buffer descriptors. */
    if (inhandles || outhandles || nscalars != nbufs)
        return Q8B_NPU_PAS_UNSUPPORTED;
    meta_size = (uint64_t)nscalars * (16u + 8u + 16u) +
            16u * sizeof(uint64_t) + 64u * sizeof(uint32_t);
    if (nbufs > NPU_FRPC_MAX_RBUFS || nscalars > NPU_FRPC_MAX_RBUFS ||
            meta + meta_size > size)
        return Q8B_NPU_PAS_INVALID;
    for (uint32_t i = 0; i < nbufs; i++) {
        uint64_t pv, len;
        memcpy(&pv, payload + meta + i * 16u, sizeof(pv));
        memcpy(&len, payload + meta + i * 16u + 8u, sizeof(len));
        if (len && !q8b_frpc_host_pointers(payload) &&
                (!pv || !npu_frpc_iova_owned(npu_frpc_session, pv, len)))
            return Q8B_NPU_PAS_INVALID;
    }
    /* The page list is authoritative for the DMA backing of every remote
     * pointer.  Validate it separately from remote_buf.pv: with standard
     * INIT_MMAP, pv may be a DSP virtual address while pages.addr remains
     * the AP consolidated SID|IOVA. */
    {
        uint32_t page_off = meta + nbufs * 16u + nbufs * 8u;
        for (uint32_t i = 0; i < nbufs; i++) {
            uint32_t list_num, list_pgidx;
            uint64_t page_addr, page_size, buf_len;
            uint32_t list_off = meta + nbufs * 16u + i * 8u;
            if ((uint64_t)list_off + 8u > size)
                return Q8B_NPU_PAS_INVALID;
            memcpy(&list_num, payload + list_off, sizeof(list_num));
            memcpy(&list_pgidx, payload + list_off + 4u,
                    sizeof(list_pgidx));
            memcpy(&buf_len, payload + meta + i * 16u + 8u,
                    sizeof(buf_len));
            /* The compact client emits one contiguous page region for each
             * buffer, with the page-list index equal to the scalar index.
             * Reject malformed metadata before handing it to GLINK; this
             * mirrors fastrpc_get_args()'s list/page relationship and keeps a
             * stale descriptor from escaping the AP SMMU ownership table. */
            if (list_pgidx != i || list_num > 1u ||
                    (!!buf_len != !!list_num))
                return Q8B_NPU_PAS_INVALID;
            if ((uint64_t)page_off + (uint64_t)(i + 1u) * 16u > size)
                return Q8B_NPU_PAS_INVALID;
            memcpy(&page_addr, payload + page_off + i * 16u,
                    sizeof(page_addr));
            memcpy(&page_size, payload + page_off + i * 16u + 8u,
                    sizeof(page_size));
            if (!!buf_len != !!page_size)
                return Q8B_NPU_PAS_INVALID;
            if (page_size && (!page_addr ||
                    !npu_frpc_iova_owned(npu_frpc_session, page_addr,
                        page_size)))
                return Q8B_NPU_PAS_INVALID;
            if (page_size) {
                /* Save validated membership while the owner buffer is
                 * live. In-flight checks must not dereference it later. */
                for (unsigned j = 0; j < NPU_FRPC_MAP_MAX; j++) {
                    const npu_frpc_map_t *m = &npu_frpc_maps[j];
                    uint64_t begin = ((uint64_t)NPU_FRPC_SID << 32) | m->iova;
                    if (m->used && page_addr >= begin &&
                            page_addr - begin <= m->size &&
                            page_size <= m->size - (page_addr - begin))
                        map_mask |= 1ull << j;
                }
            }
            if (buf_len && q8b_frpc_host_pointers(payload)) {
                uint64_t pv;
                int paired = 0;
                memcpy(&pv, payload + meta + i * 16u, sizeof(pv));
                for (uint32_t j = 0; j < NPU_FRPC_MAP_MAX; j++) {
                    npu_frpc_map_t *m = &npu_frpc_maps[j];
                    if (m->used && m->session == npu_frpc_session &&
                            q8b_frpc_buffer_matches(pv, buf_len,
                                page_addr, page_size, dma_v_addr(0, m->pa),
                                ((uint64_t)NPU_FRPC_SID << 32) | m->iova,
                                m->dsp_vaddr, m->size)) {
                        paired = 1;
                        break;
                    }
                }
                if (!paired) return Q8B_NPU_PAS_INVALID;
            }
        }
    }
    {
        uint32_t method = (sc >> 24) & 0x1fu;
        /* fastrpc_init_create_process() sets fl->pd before sending method 6,
         * so the INIT_CREATE request itself is already tagged USER_PD. */
        if (handle == 1u && (method == 6u || method == 7u))
            npu_frpc_pd = 1u;
    }
    if (q8b_frpc_next_context(&npu_frpc_sequence, npu_frpc_pd, &ctx) != 0)
        return Q8B_NPU_PAS_FAILED;
    /* Linux's kernel=true root calls route via message client_id=0.
     * Their input descriptor still names the user client (1); normal
     * user/skel invokes carry that user id in the message header too. */
    msg.client_id = q8b_frpc_message_client_id(handle);
    /* Linux sends current->pid, so its dedicated listener thread has a
     * different DSP worker from the application thread blocked in dlopen.
     * Q8B pumps both from one AP thread: reserve stable virtual thread ids. */
    msg.tid = q8b_frpc_message_tid(handle, listener);
    msg.ctx = ctx;
    msg.handle = handle;
    msg.sc = sc;
    msg.addr = msg_iova + meta;
    msg.size = (size - meta + 0xfffu) & ~0xfffull;
    if (!npu_frpc_iova_owned(npu_frpc_session, msg.addr, msg.size))
        return Q8B_NPU_PAS_INVALID;
    *pending = (npu_frpc_pending_t){ .ctx = ctx, .pa = pa, .maps = map_mask,
        .size = size, .handle = handle, .sc = sc, .submitted = npu_frpc_ticks() };
    if(glink_send_data(&msg, sizeof(msg)) != 0) {
        memset(pending, 0, sizeof(*pending));
        return Q8B_NPU_PAS_BUSY;
    }
    if(!npu_glink.metadata_logged) {
        npu_debug("q8b npu: FastRPC metadata offset=%u msg.addr=%llx size=%llu page_aligned=%d host_pv=%d\n",
                meta, (unsigned long long)msg.addr,
                (unsigned long long)msg.size, !(msg.addr & 0xfffu),
                q8b_frpc_host_pointers(payload));
        npu_glink.metadata_logged = 1;
    }
    if(listener && handle == 1u) npu_glink.listener_root_submits++;
    return 0;
#endif
}

static int npu_frpc_poll(ewokos_addr_t pa, uint32_t size) {
    if (!booted || !npu_frpc_session)
        return Q8B_NPU_PAS_UNSUPPORTED;
    if (!owned_dma(pa, size, NPU_MAX_FW, 0))
        return Q8B_NPU_PAS_INVALID;
    /* Retain even a completed packet on fatal/watchdog until PAS_STOP has
     * quiesced the DSP. Never consume GLINK responses after a known crash. */
    if(npu_smp2p_refresh_state() & Q8B_NPU_STATE_FATAL)
        return Q8B_NPU_PAS_FAILED;
    if (npu_glink.desc) (void)glink_poll();
    for (unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++) {
        npu_frpc_pending_t *p = &npu_glink.pending[i];
        if (!p->ctx || p->pa != pa || p->size != size) continue;
        if (!p->done) {
            return Q8B_NPU_PAS_BUSY;
        }
        uint32_t handle = p->handle, sc = p->sc;
        int32_t rv = p->retval;
        uint8_t *payload = (uint8_t *)dma_v_addr(0, pa);
        uint64_t iova = npu_frpc_map_lookup(npu_frpc_session, pa, size);
        __asm__ volatile("dmb sy" ::: "memory");
        memcpy(payload + 4, &rv, 4);
        if (rv == 0) {
            npu_frpc_record_dsp_mmap(npu_frpc_session, payload, iova, size);
            npu_frpc_release_dsp_mmap(npu_frpc_session, payload, iova,
                    size, handle, sc);
        }
        memset(p, 0, sizeof(*p));
        return 0;
    }
    return Q8B_NPU_PAS_INVALID;
}

static int npu_pas_prepare(void) {
    if(pas_prepared) return 0;
    if(npu_watchdog_prepare() != 0) return Q8B_NPU_PAS_FAILED;
    if(nsp_power_vote() != 0) {
        npu_recovery_required = 1;
        return Q8B_NPU_PAS_FAILED;
    }
    npu_ipcc_enable(0);
    npu_ipcc_enable(2);
    smem_prepare();
    *(volatile uint32_t *)(MMIO_BASE + 0x408000 + 0x0c) = 0x00060002u;
    __asm__ volatile("dsb sy" ::: "memory");
    pas_prepared = 1;
    return 0;
}

static int npu_pas_stage(ewokos_addr_t pa, uint32_t size) {
    proc_t *p = proc_get_proc(get_current_proc());
    page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;
    uint8_t *src;
    uint8_t *dst;

    if(size != Q8B_NSP0_SPAN || !owned_dma(pa, size, Q8B_NSP0_SPAN, 0))
        return Q8B_NPU_PAS_INVALID;
    src = (uint8_t *)dma_v_addr(0, pa);
    if(!src) return Q8B_NPU_PAS_INVALID;
    if(map_pages_size(_kernel_info.kernel_vm, Q8B_NSP_WIN,
                Q8B_NSP0_CARVEOUT, Q8B_NSP0_SPAN, AP_RW_D,
                PTE_ATTR_SYS_DMA) != 0)
        return Q8B_NPU_PAS_FAILED;
    if(pvm && pvm != _kernel_info.kernel_vm &&
            map_pages_size(pvm, Q8B_NSP_WIN, Q8B_NSP0_CARVEOUT,
                Q8B_NSP0_SPAN, AP_RW_D, PTE_ATTR_SYS_DMA) != 0)
        return Q8B_NPU_PAS_FAILED;
    flush_tlb();
    dst = (uint8_t *)Q8B_NSP_WIN;
    memcpy(dst, src, size);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

uint64_t platform_control_npu(uint64_t op, uint64_t arg, uint64_t size) {
#ifdef Q8B_QEMU_TEST
    (void)op; (void)arg; (void)size;
    return Q8B_NPU_PAS_UNSUPPORTED;
#else
    if(op == Q8B_NPU_SYS_RESET) {
        uint64_t regs[8] = { 0x84000009ull, 0, 0, 0, 0, 0, 0, 0 };
        printf("q8b npu: PSCI SYSTEM_RESET\n");
        __asm__ volatile("dsb sy" ::: "memory");
        q8b_scm_smc(regs);
        printf("q8b npu: PSCI reset returned a0=%llx\n",
                (unsigned long long)regs[0]);
        return Q8B_NPU_PAS_FAILED;
    }

    if(op == Q8B_NPU_PAS_QUERY) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        if(!probed) {
            probe_rc = nsp_probe();
            probed = 1;
        }
        if(probe_rc == Q8B_GPU_PAS_BUSY || probe_rc == Q8B_GPU_PAS_WAITQ)
            return probe_rc;
        if(probe_rc) return Q8B_NPU_PAS_UNSUPPORTED;
        return Q8B_NPU_ABI;
    }

    /* Keep the transport probe separate from PAS.  A positive result means
     * the synchronous GLINK/FastRPC service is compiled in; SESSION still
     * requires an authenticated DSP, remote intent and a ready context bank. */
    if(op == Q8B_NPU_FRPC_QUERY) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        /* The service is available once the kernel transport is present.  A
         * later SESSION request still requires PAS_BOOT to have completed, so
         * probing the QNN backend cannot accidentally touch an unbooted DSP. */
        return Q8B_NPU_FRPC_ABI;
    }

    if(op == Q8B_NPU_FRPC_POINTER_QUERY) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        return Q8B_NPU_FRPC_POINTER_HOST;
    }

    if(op == Q8B_NPU_FRPC_METADATA_QUERY) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        return Q8B_NPU_FRPC_METADATA_PAGE;
    }

    if(q8b_npu_dump_is_query(op, arg, size)) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        return Q8B_NPU_DUMP_ABI;
    }

    if(op == Q8B_NPU_FRPC_DMA_QUERY) {
        int32_t owner;
        uint32_t pins;
        if(size || !arg || (arg & 0xfffull)) return Q8B_NPU_PAS_INVALID;
        if(dma_pin_query(0, (ewokos_addr_t)arg, &owner, &pins) != 0)
            return Q8B_NPU_PAS_UNSUPPORTED;
        return ((uint64_t)pins << 32) | (uint32_t)owner;
    }

    if(op == Q8B_NPU_PAS_STATUS) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        uint32_t state = booted ? npu_smp2p_refresh_state() : 0;
        return state | (npu_recovery_required ?
                Q8B_NPU_STATE_RECOVERY_REQUIRED : 0);
    }

    if(op == Q8B_NPU_DUMP_READ) {
        if(!q8b_npu_dump_allowed(npu_dump_available, booted,
                    pas_prepared || pas_init_done || pas_mem_done || pas_auth_done))
            return Q8B_NPU_PAS_UNSUPPORTED;
        if(!q8b_npu_dump_range(size)) return Q8B_NPU_PAS_INVALID;
        uint32_t length = (uint32_t)size, offset = (uint32_t)(size >> 32);
        if(!owned_dma(arg, length, Q8B_NPU_DUMP_CHUNK, 0))
            return Q8B_NPU_PAS_INVALID;
        uint8_t *out = (uint8_t *)dma_v_addr(0, arg);
        if(!out || map_in_caller(Q8B_NSP_WIN, Q8B_NSP0_CARVEOUT,
                    (uint32_t)Q8B_NSP0_SPAN) != 0)
            return Q8B_NPU_PAS_FAILED;
        /* Like qcom_pas_da_to_va/segment_dump, but only the fixed NSP0
         * carveout after secure shutdown, into this caller's owned DMA. */
        const volatile uint8_t *source =
                (const volatile uint8_t *)(Q8B_NSP_WIN + offset);
        for(uint32_t i = 0; i < length; i++) out[i] = source[i];
        __asm__ volatile("dsb sy" ::: "memory");
        return 0;
    }

    if(npu_recovery_required && (op == Q8B_NPU_PAS_INIT ||
                op == Q8B_NPU_PAS_MEM || op == Q8B_NPU_PAS_STAGE ||
                op == Q8B_NPU_PAS_BOOT || op == Q8B_NPU_PAS_AUTH ||
                op == Q8B_NPU_FRPC_SESSION || op == Q8B_NPU_FRPC_SUBMIT ||
                op == Q8B_NPU_FRPC_SUBMIT_LISTENER)) {
        printf("q8b npu: restart blocked after DSP failure; board reset required\n");
        return Q8B_NPU_PAS_FAILED;
    }

    if(op == Q8B_NPU_SMMU_READ_ID) {
        if(size || (arg != 0x20u && arg != 0x24u && arg != 0x28u))
            return Q8B_NPU_PAS_INVALID;
        return (uint64_t)q8b_npu_smmu_read_id((uint32_t)arg);
    }

    if(op == Q8B_NPU_SMMU_CHECK) {
        if(arg || size) return Q8B_NPU_PAS_INVALID;
        if(booted || npu_frpc_session) return Q8B_NPU_PAS_BUSY;
        int rc = q8b_npu_smmu_enable();
        int stop_rc = q8b_npu_smmu_stop();
        return rc == 0 && stop_rc == 0 ? 0 : Q8B_NPU_PAS_FAILED;
    }

    if(op == Q8B_NPU_FRPC_SESSION) {
        if (npu_frpc_session) return Q8B_NPU_PAS_BUSY;
        if(size || arg != 3 || !booted)
            return Q8B_NPU_PAS_UNSUPPORTED;
        /* Check before ping can reinitialize the shared queues, then
         * recheck below in case state changes during the handshake. */
        if(!q8b_npu_state_ready(npu_smp2p_refresh_state()))
            return Q8B_NPU_PAS_UNSUPPORTED;
        if(!npu_glink.ready || !npu_glink.cid) {
            glink_ping();
            if(!npu_glink.cid) return Q8B_NPU_PAS_UNSUPPORTED;
        }
        {
            uint32_t state = npu_smp2p_refresh_state();
            if(!q8b_npu_state_ready(state))
                return Q8B_NPU_PAS_UNSUPPORTED;
        }
        /* A remote intent is required before any FastRPC TX_DATA can be
         * accepted.  Do one bounded poll during the initial ping; report a
         * clean decline when the firmware does not advertise an intent
         * rather than making every userspace retry block for seconds. */
        if(!npu_glink.remote_valid)
            return Q8B_NPU_PAS_UNSUPPORTED;
        if (q8b_npu_smmu_enable() != 0)
            return Q8B_NPU_PAS_UNSUPPORTED;
        /* DT compute-cb@3 is the CDSP session.  This is the SID inserted in
         * every DSP-visible address by upstream fastrpc.c. */
        npu_frpc_pd = 0;
        npu_frpc_session = NPU_FRPC_SID;
        return npu_frpc_session;
    }

    if(op == Q8B_NPU_FRPC_CLOSE) {
        if(size || arg != npu_frpc_session) return Q8B_NPU_PAS_INVALID;
        for (unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++)
            if (npu_glink.pending[i].ctx) return Q8B_NPU_PAS_BUSY;
        if (q8b_npu_smmu_stop() != 0) return Q8B_NPU_PAS_FAILED;
        if (npu_frpc_release_maps() != 0) return Q8B_NPU_PAS_FAILED;
        npu_frpc_pd = 0;
        npu_frpc_session = 0;
        return 0;
    }

    if(op == Q8B_NPU_FRPC_PWR) {
        if(size || !npu_frpc_session || (arg >> 32) != npu_frpc_session ||
                (arg & 0xffffffffu) > 3)
            return Q8B_NPU_PAS_INVALID;
        /* RPMh votes are held for the lifetime of PAS.  The DCVS level is
         * advisory on this first port; accepting it keeps the public FastRPC
         * ABI compatible while avoiding an unsafe second TCS transaction. */
        return 0;
    }

    if(op == Q8B_NPU_FRPC_SUBMIT || op == Q8B_NPU_FRPC_SUBMIT_LISTENER ||
            op == Q8B_NPU_FRPC_POLL) {
        if(!npu_frpc_session || !size || size > NPU_MAX_FW || (arg & 0xfffu))
            return Q8B_NPU_PAS_INVALID;
        return op == Q8B_NPU_FRPC_POLL ?
                npu_frpc_poll((ewokos_addr_t)arg, (uint32_t)size) :
                npu_frpc_submit((ewokos_addr_t)arg, (uint32_t)size,
                    op == Q8B_NPU_FRPC_SUBMIT_LISTENER);
    }

    if(op == Q8B_NPU_FRPC_MMAP) {
        uint64_t devaddr = 0;
        int ret;
        ret = npu_frpc_map(arg, (uint32_t)size, &devaddr);
        if (ret)
            return (uint64_t)(int64_t)ret;
        return devaddr;
    }

    if(op == Q8B_NPU_FRPC_MUNMAP) {
        return npu_frpc_unmap(arg, (uint32_t)size);
    }

    /* Expose the same five-step PAS ABI used by npu_cdsp.c.  npupas uses the
     * compact PAS_BOOT operation below, while the library keeps the Linux
     * qcom_q6v5_pas ordering so a future remoteproc client can reuse it. */
    if(op == Q8B_NPU_PAS_INIT) {
        uint32_t cap = 0;
        uint64_t result = 0;
        int ret;
        if(!size || !owned_dma((ewokos_addr_t)arg, size, NPU_META_SIZE,
                &cap) || cap < size)
            return Q8B_NPU_PAS_INVALID;
        npu_dump_available = 0;
        if(npu_pas_prepare() != 0) return Q8B_NPU_PAS_FAILED;
        ret = scm_call(0x201, 0x82, Q8B_NSP0_PAS_ID, arg,
                (uint32_t)size, 0, &result);
        if(ret || result) return ret ? Q8B_NPU_PAS_FAILED : Q8B_NPU_PAS_FAILED;
        pas_init_done = 1;
        return 0;
    }

    if(op == Q8B_NPU_PAS_MEM) {
        uint64_t result = 0;
        int ret;
        if(!pas_init_done || size || arg != Q8B_NSP0_SPAN)
            return Q8B_NPU_PAS_INVALID;
        ret = scm_call(0x202, 3, Q8B_NSP0_PAS_ID, Q8B_NSP0_CARVEOUT,
                Q8B_NSP0_SPAN, 0, &result);
        if(ret || result) return Q8B_NPU_PAS_FAILED;
        pas_mem_done = 1;
        return 0;
    }

    if(op == Q8B_NPU_PAS_STAGE) {
        if(!pas_mem_done || size != Q8B_NSP0_SPAN)
            return Q8B_NPU_PAS_INVALID;
        return npu_pas_stage((ewokos_addr_t)arg, (uint32_t)size);
    }

    if(op == Q8B_NPU_PAS_AUTH) {
        uint64_t result = 0;
        int ret;
        if(!pas_mem_done || arg || size) return Q8B_NPU_PAS_INVALID;
        npu_boot_notify_arm();
        ret = scm_call(0x205, 1, Q8B_NSP0_PAS_ID, 0, 0, 0, &result);
        if(ret || result) return Q8B_NPU_PAS_FAILED;
        booted = 1;
        npu_smp2p_state = Q8B_NPU_STATE_BOOTED;
        pas_auth_done = 1;
        if(npu_wait_for_ready() != 0) return Q8B_NPU_PAS_FAILED;
        glink_wait_for_remote_items();
        glink_ping();
        return 0;
    }

    if(op == Q8B_NPU_PAS_BOOT) {
        void *fw;
        uint32_t capacity = 0;
        if(booted) return 0;
        if(!owned_dma(arg, size, NPU_MAX_FW, &capacity) ||
                capacity < size + NPU_META_SIZE)
            return Q8B_NPU_PAS_INVALID;
        fw = (void *)dma_v_addr(0, arg);
        if(fw == 0) return Q8B_NPU_PAS_INVALID;
        return stage_and_boot(fw, (uint32_t)size, arg, capacity);
    }

    if(op == Q8B_NPU_PAS_STOP) {
        uint64_t result = 0;
        int ret, smmu_ret;

        if(arg || size) return Q8B_NPU_PAS_INVALID;
        if(!booted) {
            /* A previous secure stop may have succeeded while SMMU
             * teardown failed. Do not turn its retry into false success. */
            if(q8b_npu_smmu_stop() != 0 || npu_frpc_release_maps() != 0)
                return Q8B_NPU_PAS_FAILED;
            return 0;
        }
        if(npu_recovery_required) npu_watchdog_snapshot("before-stop");
        /* A timed-out invoke still owns its DMA packet. Preserve the queue
         * and translation fault evidence before requesting DSP shutdown. */
        for(unsigned i = 0; i < NPU_FRPC_PENDING_MAX; i++) {
            const npu_frpc_pending_t *p = &npu_glink.pending[i];
            if(!p->ctx) continue;
            uint32_t handle = p->handle, sc = p->sc;
            /* next2 is intentionally a blocking listener; a pending
             * application/root invoke is evidence of incomplete work. */
            if(!p->done && handle != 3u) npu_recovery_required = 1;
            npu_debug("q8b npu: pending ctx=%llx handle=%u sc=%08x done=%d\n",
                    (unsigned long long)p->ctx, handle, sc, p->done);
        }
        if(npu_glink.desc)
            npu_debug("q8b npu: stop GLINK tx=%u/%u rx=%u/%u remote=%u iid=%u valid=%d wait=%d rx_len=%u listener_root=%u\n",
                    npu_glink.desc[1], npu_glink.desc[0],
                    npu_glink.desc[3], npu_glink.desc[2],
                    npu_glink.remote_cid, npu_glink.remote_iid,
                    npu_glink.remote_valid, npu_glink.remote_wait_done,
                    npu_glink.rx_len, npu_glink.listener_root_submits);
        if(npu_recovery_required) glink_dump_failure_fifo();
        q8b_npu_smmu_dump_fault();
        if(npu_smp2p_refresh_state() & Q8B_NPU_STATE_FATAL) {
            npu_recovery_required = 1;
            npu_dump_crash_reason();
        }
        int sysmon_ack = !npu_recovery_required && glink_ssctl_shutdown() == 0;
        if(!sysmon_ack && npu_smp2p_request_stop() != 0) {
            npu_recovery_required = 1;
            printf("q8b npu: SMP2P stop-ack not observed; continuing PAS_STOP\n");
        }
        ret = scm_call(0x206, 1, Q8B_NSP0_PAS_ID, 0, 0, 0, &result);
        if(ret != 0 || result != 0) {
            printf("q8b npu: PAS_STOP failed ret=%d result=%llu; preserving DMA\n",
                    ret, (unsigned long long)result);
            return Q8B_NPU_PAS_FAILED;
        }
        booted = 0;
        npu_dump_available = 1;
        npu_watchdog.armed = 0; /* Preserve tripped until whole-board reset. */
        pas_prepared = 0;
        pas_init_done = 0;
        pas_mem_done = 0;
        pas_auth_done = 0;
        npu_smp2p_state = 0;
        npu_frpc_session = 0;
        /* PAS_STOP may be called directly by npupas, without a preceding
         * FastRPC CLOSE.  Do not retain PA->IOVA entries whose page tables
         * are freed below; a subsequent session must start with a fresh
         * ownership table and allocator cursor. */
        npu_frpc_pd = 0;
        memset(&npu_glink, 0, sizeof(npu_glink));
        smmu_ret = q8b_npu_smmu_stop();
        if(smmu_ret != 0) {
            npu_recovery_required = 1;
            printf("q8b npu: SMMU teardown failed; board reset required\n");
        }
        if(smmu_ret == 0 && npu_frpc_release_maps() != 0)
            smmu_ret = -1;
        if(nsp_power_release() != 0)
            return Q8B_NPU_PAS_FAILED;
        if(smmu_ret != 0) return Q8B_NPU_PAS_FAILED;
        printf("q8b npu: NSP0 stopped\n");
        return (ret != 0 || result != 0) ? Q8B_NPU_PAS_FAILED : 0;
    }

    return Q8B_NPU_PAS_UNSUPPORTED;
#endif
}
