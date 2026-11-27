/* Q8B battery-manager (ADSP PMIC GLINK) kernel door.
 *
 * Stage 1 (this file): boot the ADSP exactly once, from an owned DMA image
 * supplied by powerd, using the same SCM PAS sequence that the CDSP path in
 * bsp/npu_pas.c already proves on this board:
 *
 *   SCM 0x201 PAS_INIT(1, metadata)            metadata = ELF header + hash
 *   SCM 0x202 MEM_SETUP(carveout 0x86c00000, 0x2000000)
 *   stage relocated segments at carveout, base = min LOAD paddr
 *   SCM 0x205 AUTH_AND_RESET(1)
 *   wait for the ADSP SMP2P slave-kernel READY/HANDOVER bits
 *
 * Verified parameters (sc8280xp.dtsi + qcom_q6v5_pas.c sm8250_adsp_resource):
 *   remoteproc 0x03000000, pas_id 1, crash smem 423, ssr "lpass",
 *   proxy power domains lcx/lmx, memory-region pil_adsp_mem
 *   0x86c00000 + 0x2000000 (no-map, Linux /proc/iomem confirms this range).
 *   Firmware qcadsp8280.mbn is ELF32 PIL, RELOCATABLE, LOAD paddr spans
 *   0x86a00000..0x88a00000 (span == carveout size), so the carving base for
 *   staged offsets is the lowest paddr, never the carveout address itself.
 *   SMP2P: qcom,smem = <443>, <429> (in first), IPCC client LPASS(3),
 *   signals GLINK_QMP(0) / SMP2P(2), remote pid 2.
 *
 * GLINK channel "PMIC_RTR_ADSP_APPS", PDR "msm/adsp/charger_pd" and message
 * relay are stage 2; SEND/RECV stay Q8B_BATT_UNSUPPORTED until then so powerd
 * keeps its proven SPMI-only sources.
 */
#include <stdint.h>
#include <kernel/proc.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/dma.h>
#include <mm/mmu.h>
#include <kprintf.h>
#include <q8b_batt.h>
#include <q8b_mdt_load.h>

#ifndef Q8B_QEMU_TEST
/* Shared with bsp/npu_pas.c (thin wrappers over the verified CDSP path). */
extern int q8b_shared_scm(uint32_t fn, uint32_t info, uint64_t a, uint64_t b,
        uint64_t c, int fast, uint64_t *result);
extern int q8b_shared_scm_probe(void);
extern void q8b_shared_ipcc_enable(uint32_t client, uint32_t signal);
extern int q8b_shared_rpmh_begin(void);
extern int q8b_shared_rpmh_vote(const char *name);
extern int q8b_shared_rpmh_bcm_max(const char *name);

#define ADSP_PAS_ID       1u
#define ADSP_CARVEOUT     0x86c00000ull
#define ADSP_SPAN         0x2000000u
#define ADSP_WIN          0x5006000000ull  /* kernel-only window (NPU uses 0x5000..) */
#define ADSP_META_MAX     0x4000u
#define ADSP_MAX_FW       (16u * 1024u * 1024u)

#define ADSP_SMEM_PHY     0x80900000ull
#define ADSP_SMEM_SIZE    0x200000ull
#define ADSP_SMEM_WIN     0x5020000000ull

#define ADSP_SMP2P_IN     443u
#define ADSP_SMP2P_OUT    429u
#define ADSP_SMP2P_DATA   0x154u  /* 20-byte header + 16 * 20-byte entries */
#define ADSP_SMP2P_ITEM   0x158u  /* 8-byte aligned; 4 pad bytes */

#define ADSP_IPCC_CLIENT  3u      /* IPCC_CLIENT_LPASS */
#define ADSP_SIG_GLINK    0u      /* IPCC_MPROC_SIGNAL_GLINK_QMP */
#define ADSP_SIG_SMP2P    2u      /* IPCC_MPROC_SIGNAL_SMP2P */
#define ADSP_REMOTE_PID   2u
#define ADSP_LOCAL_PID    0u
#define IPCC_KICK         (ADSP_IPCC_CLIENT << 16 | ADSP_SIG_SMP2P)

#define ADSP_SMP2P_MAGIC  0x504d5324u
#define SMEM_PRIVATE_CANARY 0xa5a5u

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} batt_elf32_hdr;

static int s_booted;
static uint32_t s_caps;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
            (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int map_window(ewokos_addr_t va, ewokos_addr_t pa, uint32_t size,
        uint32_t attr) {
    proc_t* p = proc_get_proc(get_current_proc());
    page_dir_entry_t* pvm = (p && p->space) ? p->space->vm : 0;

    if (map_pages_size(_kernel_info.kernel_vm, va, pa, size, AP_RW_D, attr) != 0)
        return -1;
    if (pvm != 0 && pvm != _kernel_info.kernel_vm &&
            map_pages_size(pvm, va, pa, size, AP_RW_D, attr) != 0)
        return -1;
    flush_tlb();
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static void batt_delay_ms(uint32_t ms) {
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    do {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    } while ((now - start) * 1000ull < freq * ms);
}

/* ---- SMEM: locate the ADSP partition and its SMP2P items ------------- */
static uint8_t* smem_probe_smem(void) {
    if (map_window(ADSP_SMEM_WIN, ADSP_SMEM_PHY, ADSP_SMEM_SIZE,
                PTE_ATTR_SYS_DMA) != 0) {
        printf("q8b batt: SMEM mapping failed\n");
        return 0;
    }
    return (uint8_t*)(uintptr_t)ADSP_SMEM_WIN;
}

/* "\$PRT" partition containing either ADSP SMP2P item. */
static uint8_t* smem_adsp_partition(uint8_t* s) {
    for (uint32_t off = 0x1000u; off + 0x1000u <= ADSP_SMEM_SIZE; off += 0x1000u) {
        uint8_t* ph = s + off;
        if (ph[0] != '$' || ph[1] != 'P' || ph[2] != 'R' || ph[3] != 'T')
            continue;
        uint32_t size = rd32(ph + 8), fU = rd32(ph + 12);
        if (size < 32u || fU > size || off + size > ADSP_SMEM_SIZE)
            continue;
        for (uint32_t e = 32u; e + 16u <= fU; ) {
            uint8_t* it = ph + e;
            uint16_t canary = (uint16_t)(it[0] | it[1] << 8);
            uint16_t item = (uint16_t)(it[2] | it[3] << 8);
            uint32_t isz = rd32(it + 4);
            uint16_t pad_hdr = (uint16_t)(it[10] | it[11] << 8);
            if (canary != SMEM_PRIVATE_CANARY || isz > 0x10000u || pad_hdr > 0x1000u)
                break;
            if (item == ADSP_SMP2P_IN || item == ADSP_SMP2P_OUT)
                return ph;
            e += 16u + pad_hdr + isz;   /* size already includes padding_data */
        }
    }
    return 0;
}

static uint8_t* smem_find_item(uint8_t* ph, uint16_t want, uint32_t* size) {
    uint32_t fU = rd32(ph + 12);
    for (uint32_t e = 32u; e + 16u <= fU; ) {
        uint8_t* it = ph + e;
        uint16_t canary = (uint16_t)(it[0] | it[1] << 8);
        uint16_t item = (uint16_t)(it[2] | it[3] << 8);
        uint32_t isz = rd32(it + 4);
        uint16_t pad_hdr = (uint16_t)(it[10] | it[11] << 8);
        if (canary != SMEM_PRIVATE_CANARY || isz > 0x10000u || pad_hdr > 0x1000u)
            break;
        if (item == want) {
            if (size) *size = isz;
            return it + 16u + pad_hdr;
        }
        e += 16u + pad_hdr + isz;   /* size already includes padding_data */
    }
    return 0;
}

/* Append one private item when the boot firmware did not pre-create it.
 * Only used for the AP-owned outbound SMP2P edge (item 429). */
static uint8_t* smem_alloc_item(uint8_t* ph, uint16_t item, uint32_t size) {
    uint32_t fU = rd32(ph + 12), size_total = rd32(ph + 8);
    uint32_t need = 16u + ((size + 7u) & ~7u);
    uint8_t* e;
    if (fU + need > size_total) return 0;
    e = ph + fU;
    e[0] = (uint8_t)(SMEM_PRIVATE_CANARY & 0xff);
    e[1] = (uint8_t)(SMEM_PRIVATE_CANARY >> 8);
    e[2] = (uint8_t)(item & 0xff);
    e[3] = (uint8_t)(item >> 8);
    wr32(e + 4, size);
    e[8] = 0; e[9] = 0;      /* padding_data */
    e[10] = 0; e[11] = 0;    /* padding_hdr */
    wr32(e + 12, 0);         /* reserved */
    __asm__ volatile("dsb sy" ::: "memory");
    wr32(ph + 12, fU + need);
    __asm__ volatile("dsb sy" ::: "memory");
    return e + 16u;
}

/* master-kernel item: local=AP(0), remote=ADSP(2); version published last. */
static void smp2p_init_master(uint8_t* d, uint32_t size) {
    static const char name[] = "master-kernel";
    uint32_t total = (size - 20u) / 20u;
    for (uint32_t i = 0; i < size; i++) d[i] = 0;
    wr32(d, ADSP_SMP2P_MAGIC);
    d[4] = 0;                 /* version: publish after the header */
    d[5] = 1; d[6] = 0; d[7] = 0;  /* features: SSR_ACK */
    d[8] = (uint8_t)(ADSP_LOCAL_PID & 0xff);
    d[9] = (uint8_t)(ADSP_LOCAL_PID >> 8);
    d[10] = (uint8_t)(ADSP_REMOTE_PID & 0xff);
    d[11] = (uint8_t)(ADSP_REMOTE_PID >> 8);
    d[12] = (uint8_t)(total & 0xff);
    d[13] = (uint8_t)(total >> 8);
    d[14] = 1; d[15] = 0;     /* valid_entries = 1 */
    wr32(d + 16, 0);
    for (uint32_t i = 0; i < sizeof(name); i++) d[20 + i] = (uint8_t)name[i];
    __asm__ volatile("dsb sy" ::: "memory");
    d[4] = 2;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Read the ADSP slave-kernel bits (0 fatal, 1 ready, 2 handover, 3 stop-ack). */
static int smp2p_read_slave(uint8_t* ph, uint32_t* bits) {
    static const char name[] = "slave-kernel";
    uint32_t size = 0, total, valid;
    uint8_t* d = smem_find_item(ph, ADSP_SMP2P_IN, &size);
    if (!d || rd32(d) != ADSP_SMP2P_MAGIC || (d[4] != 1 && d[4] != 2) || size < 20u)
        return -1;
    total = (uint32_t)d[12] | (uint32_t)d[13] << 8;
    valid = (uint32_t)d[14] | (uint32_t)d[15] << 8;
    if (!total || total > 16u || valid > total || size < 20u + total * 20u)
        return -1;
    for (uint32_t i = 0; i < valid; i++) {
        uint8_t* ent = d + 20u + i * 20u;
        unsigned k;
        for (k = 0; k < sizeof(name); k++)
            if (ent[k] != (uint8_t)name[k]) break;
        if (k == sizeof(name)) {
            *bits = rd32(ent + 16);
            return 0;
        }
    }
    return -1;
}

static int smp2p_arm(uint8_t* s) {
    uint8_t* ph = smem_adsp_partition(s);
    uint8_t* out;
    uint32_t have = 0;
    if (!ph) {
        printf("q8b batt: no SMEM partition carrying ADSP SMP2P items\n");
        return -1;
    }
    out = smem_find_item(ph, ADSP_SMP2P_OUT, &have);
    if (!out || have < 0x154u) {
        out = smem_alloc_item(ph, ADSP_SMP2P_OUT, ADSP_SMP2P_ITEM);
        if (!out) {
            printf("q8b batt: SMP2P item 429 missing and cannot be allocated\n");
            return -1;
        }
        printf("q8b batt: allocated SMP2P item 429 in SMEM\n");
    }
    smp2p_init_master(out, ADSP_SMP2P_ITEM);
    *(volatile uint32_t*)(MMIO_BASE + 0x408000 + 0x0c) = IPCC_KICK;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

/* ---- ADSP PAS boot --------------------------------------------------- */
static int adsp_power_on(void) {
    if (q8b_shared_rpmh_begin() != 0) {
        printf("q8b batt: rpmh begin failed\n");
        return -1;
    }
    /* sm8250_adsp_resource proxy domains: lcx + lmx; XO for early SMEM. */
    if (q8b_shared_rpmh_vote("xo.lvl") != 0 ||
            q8b_shared_rpmh_vote("lcx.lvl") != 0 ||
            q8b_shared_rpmh_vote("lmx.lvl") != 0) {
        printf("q8b batt: ADSP power vote failed\n");
        return -1;
    }
    return 0;
}

static uint64_t adsp_boot(void* fw, uint32_t fw_size, uint64_t fw_pa,
        uint32_t capacity) {
    const batt_elf32_hdr* eh = (const batt_elf32_hdr*)fw;
    const q8b_mdt_phdr* ph;
    uint64_t min_addr = ~0ull, max_addr = 0, meta_len;
    uint32_t i, hash = 0, meta_off;
    uint8_t* meta;
    uint64_t result = 0;
    int ret;
    uint8_t* smem;

    if (fw_size < sizeof(*eh) || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
            eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_ident[4] != 1)
        return Q8B_BATT_INVALID;
    if (eh->e_phnum < 2 || eh->e_phnum > 32 ||
            eh->e_phentsize != sizeof(q8b_mdt_phdr) ||
            (uint64_t)eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(q8b_mdt_phdr) > fw_size)
        return Q8B_BATT_INVALID;
    ph = (const q8b_mdt_phdr*)((const uint8_t*)fw + eh->e_phoff);

    for (i = 0; i < eh->e_phnum; i++) {
        uint64_t end;
        if (!q8b_mdt_loadable(&ph[i])) {
            if (!hash && (ph[i].p_flags & 0x07000000u) == 0x02000000u)
                hash = i;
            continue;
        }
        end = (uint64_t)ph[i].p_paddr + ph[i].p_memsz;
        if (ph[i].p_paddr < min_addr) min_addr = ph[i].p_paddr;
        if (end > max_addr) max_addr = end;
    }
    if (!hash || min_addr == ~0ull || max_addr <= min_addr)
        return Q8B_BATT_INVALID;
    max_addr = (max_addr + 0xfff) & ~0xfffull;
    if (max_addr - min_addr != ADSP_SPAN) {
        printf("q8b batt: ADSP image span %llx != carveout %x\n",
                (unsigned long long)(max_addr - min_addr), ADSP_SPAN);
        return Q8B_BATT_INVALID;
    }
    if (q8b_mdt_validate_loads(fw_size, ph, eh->e_phnum, min_addr, ADSP_SPAN) != 0)
        return Q8B_BATT_INVALID;

    /* metadata = ELF header block + hash segment, placed after the image in
     * the caller's DMA buffer so it stays mapped in this syscall's tables. */
    meta_len = (uint64_t)ph[0].p_filesz + ph[hash].p_filesz;
    meta_off = (fw_size + 15u) & ~15u;
    if (meta_len > ADSP_META_MAX || (uint64_t)meta_off + meta_len > capacity ||
            ph[0].p_filesz > fw_size ||
            (uint64_t)ph[hash].p_offset + ph[hash].p_filesz > fw_size)
        return Q8B_BATT_INVALID;
    meta = (uint8_t*)fw + meta_off;
    for (i = 0; i < ph[0].p_filesz; i++) meta[i] = ((const uint8_t*)fw)[i];
    for (i = 0; i < ph[hash].p_filesz; i++)
        meta[ph[0].p_filesz + i] = ((const uint8_t*)fw)[ph[hash].p_offset + i];
    __asm__ volatile("dsb sy" ::: "memory");

    if (q8b_shared_scm_probe() != 0) {
        printf("q8b batt: SCM probe failed\n");
        return Q8B_BATT_FAILED;
    }

    ret = q8b_shared_scm(0x201, 0x82, ADSP_PAS_ID, fw_pa + meta_off,
            (uint32_t)meta_len, 0, &result);
    printf("q8b batt: adsp PAS_INIT ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if (ret || result) return Q8B_BATT_FAILED;

    ret = q8b_shared_scm(0x202, 3, ADSP_PAS_ID, ADSP_CARVEOUT, ADSP_SPAN, 0,
            &result);
    printf("q8b batt: adsp PAS_MEM_SETUP ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if (ret || result) return Q8B_BATT_FAILED;

    if (map_window(ADSP_WIN, ADSP_CARVEOUT, ADSP_SPAN, PTE_ATTR_SYS_DMA) != 0) {
        printf("q8b batt: ADSP carveout mapping failed\n");
        return Q8B_BATT_FAILED;
    }

    smem = smem_probe_smem();
    if (!smem || smp2p_arm(smem) != 0)
        return Q8B_BATT_FAILED;

    q8b_shared_ipcc_enable(ADSP_IPCC_CLIENT, ADSP_SIG_GLINK);
    q8b_shared_ipcc_enable(ADSP_IPCC_CLIENT, ADSP_SIG_SMP2P);

    if (q8b_mdt_stage((volatile uint8_t*)(uintptr_t)ADSP_WIN, ADSP_SPAN,
                (const uint8_t*)fw, fw_size, ph, eh->e_phnum, min_addr) != 0) {
        printf("q8b batt: ADSP image staging failed\n");
        return Q8B_BATT_FAILED;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    printf("q8b batt: ADSP image staged (base=%llx span=%x)\n",
            (unsigned long long)min_addr, ADSP_SPAN);

    ret = q8b_shared_scm(0x205, 1, ADSP_PAS_ID, 0, 0, 0, &result);
    printf("q8b batt: adsp AUTH_AND_RESET ret=%d result=%llu\n", ret,
            (unsigned long long)result);
    if (ret || result) return Q8B_BATT_FAILED;

    /* ADSP handover: slave-kernel READY (bit 1) + HANDOVER (bit 2);
     * bit 0 is FATAL, bit 3 STOP_ACK (sc8280xp dtsi order). */
    for (unsigned try = 0; try < 100u; try++) {
        uint32_t bits = 0;
        if (smp2p_read_slave(smem_adsp_partition(smem), &bits) == 0) {
            if (bits & 0x2u) {  /* READY */
                printf("q8b batt: ADSP ready (slave bits %x)\n", bits);
                s_booted = 1;
                s_caps |= Q8B_BATT_CAP_ADSP_BOOTED;
                return 0;
            }
        }
        *(volatile uint32_t*)(MMIO_BASE + 0x408000 + 0x0c) = IPCC_KICK;
        __asm__ volatile("dsb sy" ::: "memory");
        batt_delay_ms(100);
    }
    printf("q8b batt: ADSP not ready within 10s\n");
    return Q8B_BATT_FAILED;
}

static int owned_dma_check(ewokos_addr_t pa, ewokos_addr_t len, uint32_t max,
        uint32_t* capacity) {
    proc_t* p = proc_get_proc(get_current_proc());
    uint32_t cap = p ? dma_size(p->info.pid, pa) : 0;
    if (capacity) *capacity = cap;
    return len && len <= max && !(pa & 0xfff) && cap >= len;
}
#endif /* Q8B_QEMU_TEST */

ewokos_addr_t batt_control_arch(ewokos_addr_t op, ewokos_addr_t arg,
        ewokos_addr_t size) {
#ifdef Q8B_QEMU_TEST
    (void)op; (void)arg; (void)size;
    return (ewokos_addr_t)Q8B_BATT_UNSUPPORTED;
#else
    if ((op & 0xffffff00ull) != Q8B_BATT_ABI_BASE)
        return (ewokos_addr_t)Q8B_BATT_INVALID;

    if (op == Q8B_BATT_QUERY || op == Q8B_BATT_STATUS) {
        if (arg || size) return (ewokos_addr_t)Q8B_BATT_INVALID;
        return (ewokos_addr_t)s_caps;
    }

    if (op == Q8B_BATT_BOOT) {
        void* fw;
        uint32_t capacity = 0;
        if (s_booted) return 0;
        if (!owned_dma_check(arg, size, ADSP_MAX_FW, &capacity) ||
                capacity < size + ADSP_META_MAX)
            return (ewokos_addr_t)Q8B_BATT_INVALID;
        fw = (void*)dma_v_addr(arg);
        if (fw == 0) return (ewokos_addr_t)Q8B_BATT_INVALID;
        if (adsp_power_on() != 0) return (ewokos_addr_t)Q8B_BATT_FAILED;
        return (ewokos_addr_t)adsp_boot(fw, (uint32_t)size, arg, capacity);
    }

    /* Channel relay (GLINK/PDR) lands in stage 2. */
    return (ewokos_addr_t)Q8B_BATT_UNSUPPORTED;
#endif
}
