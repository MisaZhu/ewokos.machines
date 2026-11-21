#include <stdint.h>
#include <mm/mmu.h>
#include <kernel/hw_info.h>
#include "hw_arch.h"

/*
 * Minimal GICv3 driver for Tegra234.
 *
 * The platform's gic.c is a GICv2 (GICC MMIO) driver; Tegra234 has a
 * GICv3 with redistributors and the ICC_* system-register interface, so
 * the Orin BSP carries its own driver (same model as the Q8B one).
 *
 * Tegra234 specifics:
 *   - one redistributor region (0x0f440000, 2MB, 0x20000 stride); frames
 *     are matched to logical cores by MPIDR affinity (GICR_TYPER), not by
 *     index, because the Orin Nano cluster layout is 4+2 with Aff1 gaps;
 *   - SPIs are routed to the boot core's affinity (UEFI may boot on any
 *     core);
 *   - the kernel uses ICC_IAR1/EOIR1 (Group 1 non-secure).
 */

#define GICD_CTLR          0x0000
#define GICD_TYPER         0x0004
#define GICD_IGROUPR       0x0080
#define GICD_ISENABLER     0x0100
#define GICD_ICENABLER     0x0180
#define GICD_ICPENDR       0x0280
#define GICD_IPRIORITYR    0x0400
#define GICD_IROUTER       0x6000
#define GICD_IGRPMODR      0x0D00

#define GICR_CTLR          0x0000
#define GICR_TYPER         0x0008
#define GICR_WAKER         0x0014
#define GICR_SGI_BASE      0x10000
#define GICR_IGROUPR0      0x0080
#define GICR_ISENABLER0    0x0100
#define GICR_ICENABLER0    0x0180
#define GICR_ICPENDR0      0x0280
#define GICR_IPRIORITYR0   0x0400
#define GICR_IGRPMODR0     0x0D00

#define GICD_CTLR_ENABLE_G0     (1u << 0)
#define GICD_CTLR_ENABLE_G1NS   (1u << 1)
#define GICD_CTLR_ARE_NS        (1u << 4)
#define GICD_CTLR_DS            (1u << 6)
#define GICD_CTLR_RWP           (1u << 31)

#define GICR_WAKER_PROCESSOR_SLEEP (1u << 0)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 1)
#define GICR_TYPER_LAST            (1u << 4)

#define GIC_INT_MAX        1020
#define GIC_SPURIOUS       1023
#define GIC_PRIO           0xA0

#define GICD  ((volatile uint32_t*)(MMIO_BASE + ORIN_GIC_WIN_OFF + \
        (ORIN_GICD_PHY - ORIN_GIC_WIN_PHY)))
#define GICR  ((volatile uint8_t*)(MMIO_BASE + ORIN_GIC_WIN_OFF + \
        (ORIN_GICR_PHY - ORIN_GIC_WIN_PHY)))

static inline uint32_t rd32(volatile void* base, uint32_t off) {
    return *(volatile uint32_t*)((volatile uint8_t*)base + off);
}
static inline void wr32(volatile void* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)((volatile uint8_t*)base + off) = v;
}
static inline void wr64(volatile void* base, uint32_t off, uint64_t v) {
    *(volatile uint64_t*)((volatile uint8_t*)base + off) = v;
}

static inline uint64_t icc_read_sre(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(v)); /* ICC_SRE_EL1 */
    return v;
}
static inline void icc_write_sre(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(v));
}
static inline void icc_write_pmr(uint64_t v) {
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(v)); /* ICC_PMR_EL1 */
}
static inline void icc_write_ctlr(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_4, %0" :: "r"(v)); /* ICC_CTLR_EL1 */
}
static inline void icc_write_igrpen1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(v)); /* ICC_IGRPEN1_EL1 */
}
static inline uint64_t icc_read_iar1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); /* ICC_IAR1_EL1 */
    return v;
}
static inline void icc_write_eoir1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"(v));
}
static inline void icc_write_sgi1r(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(v)); /* ICC_SGI1R_EL1 */
}
static inline void isb(void) {
    __asm__ volatile("isb");
}

/* Redistributor frame per logical core, resolved by affinity walk. */
static volatile uint8_t* _gicr[ORIN_CORES];
/* Affinity value SPIs are routed to (the boot core). */
static uint64_t _spi_route_aff;
static uint32_t _boot_core;

const uint64_t orin_core_mpidr[ORIN_CORES] = {
    0x00000ULL, 0x00100ULL, 0x00200ULL, 0x00300ULL,   /* cluster0: 4 cores */
    0x10200ULL, 0x10300ULL,                           /* cluster1: 2 cores */
};

uint32_t orin_core_id(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    /* Aff0/Aff1/Aff2 (bits 23:0); Aff2 is the cluster and must be kept. */
    mpidr &= 0xFFFFFFULL;
    for (uint32_t i = 0; i < ORIN_CORES; i++) {
        if ((orin_core_mpidr[i] & 0xFFFFFFULL) == mpidr)
            return i;
    }
    return 0;
}

static void gicr_discover(void) {
    for (uint32_t i = 0; i < ORIN_CORES; i++)
        _gicr[i] = 0;

    uint32_t frames = ORIN_GICR_SIZE / ORIN_GICR_STRIDE;
    for (uint32_t f = 0; f < frames; f++) {
        volatile uint8_t* rd = GICR + (uint64_t)f * ORIN_GICR_STRIDE;
        uint32_t lo = rd32(rd, GICR_TYPER);
        uint32_t hi = rd32(rd, GICR_TYPER + 4);
        if (lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu)
            break;
        /* GICR_TYPER[63:32] is the owning PE's affinity, laid out exactly
         * like MPIDR[31:0] (Aff0 low).  Aff3 is 0 on Tegra234, so keep
         * Aff0/Aff1/Aff2 and match against the MPIDR table. */
        uint64_t aff = hi & 0xFFFFFFULL;
        for (uint32_t i = 0; i < ORIN_CORES; i++) {
            if ((orin_core_mpidr[i] & 0xFFFFFFULL) == aff) {
                _gicr[i] = rd;
                break;
            }
        }
        if (lo & GICR_TYPER_LAST)
            break;
    }
}

static volatile uint8_t* gicr_rd(uint32_t core) {
    if (core < ORIN_CORES && _gicr[core] != 0)
        return _gicr[core];
    /* Fallback: architectural stride order (should not be needed). */
    return GICR + (uint64_t)core * ORIN_GICR_STRIDE;
}

static void gicd_wait_rwp(void) {
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        if (!(rd32(GICD, GICD_CTLR) & GICD_CTLR_RWP))
            return;
    }
}

static void redist_wake(uint32_t core) {
    volatile uint8_t* rd = gicr_rd(core);

    if ((rd32(rd, GICR_TYPER) == 0xFFFFFFFFu) ||
            (rd32(rd, GICR_TYPER + 4) == 0xFFFFFFFFu))
        return; /* no redistributor here */

    uint32_t waker = rd32(rd, GICR_WAKER);
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    wr32(rd, GICR_WAKER, waker);
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        if (!(rd32(rd, GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP))
            return;
    }
}

static void redist_set_prio(volatile uint8_t* rd, uint32_t irq) {
    volatile uint8_t* p = rd + GICR_SGI_BASE + GICR_IPRIORITYR0 + irq;
    *p = GIC_PRIO;
}

/* Enable an SGI/PPI on one redistributor (INTID < 32). */
static void redist_irq_enable(uint32_t core, uint32_t irq) {
    if (irq >= 32)
        return;
    volatile uint8_t* rd = gicr_rd(core);
    volatile uint8_t* sgi = rd + GICR_SGI_BASE;

    /* Group 1 non-secure, not Group 0. */
    wr32(sgi, GICR_IGROUPR0, rd32(sgi, GICR_IGROUPR0) | (1u << irq));
    wr32(sgi, GICR_IGRPMODR0, rd32(sgi, GICR_IGRPMODR0) & ~(1u << irq));
    redist_set_prio(rd, irq);
    wr32(sgi, GICR_ISENABLER0, 1u << irq);
}

static void redist_irq_disable(uint32_t core, uint32_t irq) {
    if (irq >= 32)
        return;
    volatile uint8_t* sgi = gicr_rd(core) + GICR_SGI_BASE;
    wr32(sgi, GICR_ICENABLER0, 1u << irq);
    wr32(sgi, GICR_ICPENDR0, 1u << irq);
}

/* SPI (INTID >= 32) helpers, distributor is shared, routed to the boot core. */
static void spi_irq_enable(uint32_t irq) {
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    wr32(GICD, GICD_IGROUPR + reg * 4,
            rd32(GICD, GICD_IGROUPR + reg * 4) | (1u << bit));
    *((volatile uint8_t*)GICD + GICD_IPRIORITYR + irq) = GIC_PRIO;
    wr64(GICD, GICD_IROUTER + (uint64_t)irq * 8, _spi_route_aff);
    wr32(GICD, GICD_ISENABLER + reg * 4, 1u << bit);
}

static void spi_irq_disable(uint32_t irq) {
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    wr32(GICD, GICD_ICENABLER + reg * 4, 1u << bit);
    wr32(GICD, GICD_ICPENDR + reg * 4, 1u << bit);
}

/*
 * Distributor init. Called once on the boot core before any device IRQ is
 * enabled. Wakes the redistributors out of sleep and enables both kernel
 * interrupt sources (SGI0 for IPIs, PPI27 for the virtual timer).
 */
void gic_v3_init(void) {
    _boot_core = orin_core_id();
    _spi_route_aff = orin_core_mpidr[_boot_core] & 0xFFFFFFULL;
    gicr_discover();

    /* Bring-up diagnostics: a missing redistributor match means the GICR
     * affinity walk disagreed with the MPIDR table (wrong cluster layout). */
    uint32_t matched = 0;
    for (uint32_t c = 0; c < ORIN_CORES; c++)
        if (_gicr[c] != 0)
            matched++;
    orin_dbg_puts("orin: gic boot-core=");
    orin_dbg_dec(_boot_core);
    orin_dbg_puts(" gicr=");
    orin_dbg_dec(matched);
    orin_dbg_puts("/");
    orin_dbg_dec(ORIN_CORES);
    orin_dbg_puts(" spi-aff=");
    orin_dbg_hex(_spi_route_aff);
    orin_dbg_puts("\n");
    if (matched != ORIN_CORES)
        orin_dbg_puts("orin: WARNING gicr affinity match incomplete\n");

    /* Enable system-register access if the firmware has not done it yet. */
    uint64_t sre = icc_read_sre();
    sre |= (1u << 0);  /* SRE */
    sre |= (1u << 2);  /* DIB */
    sre |= (1u << 3);  /* DFB */
    icc_write_sre(sre);
    isb();

    uint32_t ctlr = rd32(GICD, GICD_CTLR);

    /* Take the distributor down while we change ARE/group setup. */
    wr32(GICD, GICD_CTLR, ctlr & ~(GICD_CTLR_ENABLE_G0 |
            GICD_CTLR_ENABLE_G1NS));
    gicd_wait_rwp();
    wr32(GICD, GICD_CTLR, (ctlr & ~(GICD_CTLR_ENABLE_G0 |
            GICD_CTLR_ENABLE_G1NS)) | GICD_CTLR_ARE_NS);
    gicd_wait_rwp();

    uint32_t lines = ((rd32(GICD, GICD_TYPER) & 0x1F) + 1) * 32;
    if (lines > GIC_INT_MAX)
        lines = GIC_INT_MAX;

    for (uint32_t i = 0; i < lines; i += 32) {
        wr32(GICD, GICD_IGROUPR + (i / 32) * 4, 0xFFFFFFFFu);
        /* Group 1 non-secure (not Group 1 secure). */
        wr32(GICD, GICD_IGRPMODR + (i / 32) * 4, 0);
    }
    for (uint32_t i = 32; i < lines; i++)
        *((volatile uint8_t*)GICD + GICD_IPRIORITYR + i) = GIC_PRIO;
    for (uint32_t i = 32; i < lines; i++)
        wr64(GICD, GICD_IROUTER + (uint64_t)i * 8, _spi_route_aff);

    wr32(GICD, GICD_CTLR, rd32(GICD, GICD_CTLR) | GICD_CTLR_ARE_NS |
            GICD_CTLR_ENABLE_G1NS);
    gicd_wait_rwp();

    for (uint32_t c = 0; c < ORIN_CORES; c++) {
        redist_wake(c);
        redist_irq_enable(c, ORIN_GIC_IPI_IRQ);
        if (c == _boot_core)
            redist_irq_enable(c, ORIN_GIC_TIMER_IRQ);
        else
            redist_irq_disable(c, ORIN_GIC_TIMER_IRQ);
    }
}

/*
 * Per-core CPU interface init. Must run on the target core (called from
 * irq_init_arch for the boot core and cpu_core_ready() for the APs).
 */
void gic_v3_core_init(uint32_t core) {
    uint64_t sre = icc_read_sre();
    sre |= (1u << 0);
    icc_write_sre(sre);
    isb();

    icc_write_pmr(0xFF);
    icc_write_ctlr(0);          /* EOImode=0: EOIR1 both priority-drops and deactivates */
    icc_write_igrpen1(1);
    isb();

    redist_wake(core);
    redist_irq_enable(core, ORIN_GIC_IPI_IRQ);
    if (core == _boot_core)
        redist_irq_enable(core, ORIN_GIC_TIMER_IRQ);
    else
        redist_irq_disable(core, ORIN_GIC_TIMER_IRQ);
}

void gic_v3_irq_enable(int core, int irq) {
    if (irq < 0 || irq > GIC_INT_MAX)
        return;
    if (irq < 32)
        redist_irq_enable((uint32_t)core, (uint32_t)irq);
    else
        spi_irq_enable((uint32_t)irq);
}

void gic_v3_irq_disable(int core, int irq) {
    if (irq < 0 || irq > GIC_INT_MAX)
        return;
    if (irq < 32)
        redist_irq_disable((uint32_t)core, (uint32_t)irq);
    else
        spi_irq_disable((uint32_t)irq);
}

uint32_t gic_v3_get_irq(void) {
    uint64_t iar = icc_read_iar1();
    uint32_t irq = (uint32_t)(iar & 0xFFFFFF);
    return (irq == GIC_SPURIOUS) ? 1023 : irq;
}

void gic_v3_eoi(uint32_t irq) {
    if (irq >= 1020)
        return;
    icc_write_eoir1(irq);
}

void gic_v3_send_sgi(int core, int irq) {
    if (core < 0 || core >= ORIN_CORES)
        return;
    /*
     * ICC_SGI1R_EL1: Aff0 target list in bits 15:0, Aff1 in 23:16, INTID in
     * 27:24, Aff2 in 39:32.  Orin Nano MPIDRs are 0x000..0x300 (cluster0,
     * Aff2=0) and 0x10200/0x10300 (cluster1, Aff2=1), so Aff2 MUST be set
     * or an IPI to a cluster1 core would be routed into cluster0.
     */
    uint64_t mpidr = orin_core_mpidr[core];
    uint64_t aff0 = mpidr & 0xFF;
    uint64_t aff1 = (mpidr >> 8) & 0xFF;
    uint64_t aff2 = (mpidr >> 16) & 0xFF;
    uint64_t val = (aff1 << 16) | (1ULL << aff0) |
            ((uint64_t)(irq & 0xF) << 24) | (aff2 << 32);
    /* Publish ready-queue and need_resched stores before the target handles
     * the SGI. This is the ordering required by the GICv3 architecture and
     * used by Linux's gic_ipi_send_mask(). */
    __asm__ volatile("dsb ishst" ::: "memory");
    icc_write_sgi1r(val);
    isb();
}