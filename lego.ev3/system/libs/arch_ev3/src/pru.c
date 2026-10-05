#include <stdint.h>
#include <ewoksys/mmio.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>

#include "../include/arch/ev3/pru.h"

/*
 * AM1808 (DA850) PRUSS low-level HAL for the EV3 - see pru.h for the why.
 *
 * This is a straight port of the TI StarterWare/SUART pru.c primitives
 * (pru_load / pru_run / pru_disable / pru_enable / pru_psc_enable /
 * pru_ram_{read,write}_data{,_4byte}) into EwokOS's flat-MMIO model. TI reaches
 * the blocks through an arm_pru_iomap struct of pre-mapped virtual bases; here
 * every PRUSS/McASP/PSC register lives inside the kernel's 32 MB MMIO window and
 * is simply _mmio_base + physical_offset, while the one region OUTSIDE that
 * window (the ARM<->PRU shared SRAM at 0x8000_0000) is mapped once by
 * ev3_pru_init() and cached in _shared_ram_va.
 *
 * Deliberate deviations from the TI source (both documented at the call site):
 *  - pru_load()/pru_enable() in TI redundantly re-enable the PSC and re-reset
 *    the core; here the lifecycle is split into explicit psc/reset/load/run
 *    steps so the SUART init in pru_uart.c controls the exact ordering.
 *  - TI zeroes PRU DRAM with an unaligned `*(u32*)(base | i)` loop. That is
 *    harmless on the C674x/DSP but faults-or-misbehaves on ARM926 Device
 *    memory, so ev3_pru_clear_dram() writes aligned words instead.
 */

/* Shared SRAM virtual base (set by ev3_pru_init(); 0 until then). */
static ewokos_addr_t _shared_ram_va = 0;
static int _pru_inited = 0;

/* DA850 PSC (Power/Sleep Controller) LPSC register offsets. Identical layout to
 * the copy in port.c: a peripheral gets no functional clock until its module is
 * switched to ENABLE, and an unclocked DA850 block silently discards every
 * register write - so PRUSS and McASP0 must both be ungated before use. */
#define PSC_MDSTAT(n)   (0x800 + (n) * 4)
#define PSC_MDCTL(n)    (0xA00 + (n) * 4)
#define PSC_PTCMD       0x120
#define PSC_PTSTAT      0x128
#define PSC_MD_ENABLE   0x3     /* MDSTAT/MDCTL.STATE field = ENABLE */

/* ---------------- mapping / virtual bases ---------------- */

int ev3_pru_init(void) {
    sys_info_t sysinfo;
    ewokos_addr_t target;

    if (_pru_inited)
        return 0;

    /* The PRUSS/McASP/PSC registers are reached through the global MMIO window;
     * map it if the host daemon has not already. */
    if (_mmio_base == 0) {
        if (mmio_map() == 0)
            return -1;
    }

    /* The ARM<->PRU shared SRAM at 0x8000_0000 is OUTSIDE the 32 MB MMIO window,
     * so map it just past the window end (still inside the kernel's 128 MB MMIO
     * reservation, above the kmalloc arena). The kernel whitelists this exact
     * physical range in check_mem_map_arch() and maps it Device/uncached, which
     * is what ARM<->PRU0 coherency needs. */
    sys_get_sys_info(&sysinfo);
    target = _mmio_base + sysinfo.mmio.size;
    if (syscall3(SYS_MEM_MAP, target,
                 (ewokos_addr_t)EV3_SHARED_RAM_PHYS,
                 (ewokos_addr_t)EV3_SHARED_RAM_SIZE) != target)
        return -1;
    _shared_ram_va = target;

    _pru_inited = 1;
    return 0;
}

ewokos_addr_t ev3_pru_base(void) {
    return _mmio_base + EV3_PRUSS_PHYS;
}

ewokos_addr_t ev3_pru_mcasp_base(void) {
    return _mmio_base + EV3_MCASP0_PHYS;
}

ewokos_addr_t ev3_pru_shared_ram(void) {
    return _shared_ram_va;
}

/* ---------------- PSC clock gating ---------------- */

/* Switch one LPSC module to ENABLE. Mirrors port.c's psc_module_enable(): poll
 * PTSTAT for any in-flight transition, skip if already enabled, else set
 * MDCTL.NEXT=ENABLE, poke PTCMD.GO, and wait for the state to take. Bounded
 * loops so a stuck PSC can never hang a daemon. */
static void psc_enable(uint32_t psc_phys, int module) {
    volatile uint32_t* mdctl  = (volatile uint32_t*)(_mmio_base + psc_phys + PSC_MDCTL(module));
    volatile uint32_t* mdstat = (volatile uint32_t*)(_mmio_base + psc_phys + PSC_MDSTAT(module));
    volatile uint32_t* ptcmd  = (volatile uint32_t*)(_mmio_base + psc_phys + PSC_PTCMD);
    volatile uint32_t* ptstat = (volatile uint32_t*)(_mmio_base + psc_phys + PSC_PTSTAT);
    int t;

    if ((*mdstat & 0x1f) == PSC_MD_ENABLE)
        return;                                   /* already enabled */
    t = 100000; while (*ptstat && --t > 0) ;      /* wait for in-flight transition */
    *mdctl = (*mdctl & ~0x1fu) | PSC_MD_ENABLE;   /* NEXT = ENABLE */
    *ptcmd = 0x1;                                 /* GO */
    t = 100000; while (*ptstat && --t > 0) ;
    t = 100000; while (((*mdstat & 0x1f) != PSC_MD_ENABLE) && --t > 0) ;
}

void ev3_pru_psc_enable(void) {
    psc_enable(EV3_PSC0_PHYS, EV3_PSC_MOD_PRUSS);
}

void ev3_pru_mcasp_psc_enable(void) {
    psc_enable(EV3_PSC1_PHYS, EV3_PSC_MOD_MCASP0);
}

/* ---------------- PRU core lifecycle ---------------- */

static volatile uint32_t* pru_ctrl(int pru) {
    uint32_t off = pru ? EV3_PRU1_CTRL_OFF : EV3_PRU0_CTRL_OFF;
    return (volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + off);
}

void ev3_pru_reset(int pru) {
    /* CONTROL = RESETVAL holds the core in reset and clears ENABLE/COUNTER, so
     * a subsequent IRAM load is not raced by a still-fetching core. */
    *pru_ctrl(pru) = EV3_PRU_CTRL_RESETVAL;
}

int ev3_pru_load(int pru, const uint32_t* code, uint32_t words) {
    uint32_t iram_off = pru ? EV3_PRU1_IRAM_OFF : EV3_PRU0_IRAM_OFF;
    volatile uint32_t* iram = (volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + iram_off);
    uint32_t i;

    if (code == 0 || words == 0)
        return -1;
    if (words * 4 > EV3_PRU_IRAM_SIZE)
        return -1;                                /* firmware too big for IRAM */

    for (i = 0; i < words; i++)
        iram[i] = code[i];
    return 0;
}

void ev3_pru_run(int pru) {
    /* Release the core: ENABLE starts fetch/execute from IRAM[0]; COUNTER_ENABLE
     * additionally gates the profiling cycle counter (harmless, matches TI's
     * pru_run() which sets both). */
    *pru_ctrl(pru) |= (EV3_PRU_CTRL_ENABLE | EV3_PRU_CTRL_COUNTER_ENABLE);
}

void ev3_pru_disable(int pru) {
    volatile uint32_t* c = pru_ctrl(pru);
    *c &= ~EV3_PRU_CTRL_COUNTER_ENABLE;
    *c &= ~EV3_PRU_CTRL_ENABLE;
    *c = EV3_PRU_CTRL_RESETVAL;                   /* back into reset */
}

int ev3_pru_is_running(int pru) {
    return (*pru_ctrl(pru) & EV3_PRU_CTRL_RUNSTATE) ? 1 : 0;
}

/* ---------------- PRU data-RAM access ----------------
 * off is absolute from the PRUSS base (PRU0 DRAM == offset 0). The byte helpers
 * back the SUART control fields (RX/TX mode, ISR byte) which sit at unaligned
 * DRAM offsets; the 32-bit helpers back the word-aligned INTC registers. Both
 * are plain volatile copies - _mmio_base maps this region Device/uncached so
 * there is no cache to flush and ordering is guaranteed. */

void ev3_pru_ram_write(uint32_t off, const void* data, uint32_t bytes) {
    volatile uint8_t* dst = (volatile uint8_t*)(_mmio_base + EV3_PRUSS_PHYS + off);
    const uint8_t* src = (const uint8_t*)data;
    uint32_t i;
    for (i = 0; i < bytes; i++)
        dst[i] = src[i];
}

void ev3_pru_ram_read(uint32_t off, void* data, uint32_t bytes) {
    volatile uint8_t* src = (volatile uint8_t*)(_mmio_base + EV3_PRUSS_PHYS + off);
    uint8_t* dst = (uint8_t*)data;
    uint32_t i;
    for (i = 0; i < bytes; i++)
        dst[i] = src[i];
}

void ev3_pru_ram_write32(uint32_t off, const uint32_t* data, uint32_t words) {
    volatile uint32_t* dst = (volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + off);
    uint32_t i;
    for (i = 0; i < words; i++)
        dst[i] = data[i];
}

void ev3_pru_ram_read32(uint32_t off, uint32_t* data, uint32_t words) {
    volatile uint32_t* src = (volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + off);
    uint32_t i;
    for (i = 0; i < words; i++)
        data[i] = src[i];
}

/* ---------------- PRUSS INTC register access ----------------
 * reg is relative to the INTC block (EV3_PRUSS_INTC_OFF). The INTC is the same
 * CP-INTC IP the kernel's own AINTC driver services (kernel/bsp/irq.c), so its
 * register offsets are authoritative for this chip. */

uint32_t ev3_pru_intc_read(uint32_t reg) {
    return *(volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + EV3_PRUSS_INTC_OFF + reg);
}

void ev3_pru_intc_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(_mmio_base + EV3_PRUSS_PHYS + EV3_PRUSS_INTC_OFF + reg) = val;
}

/* ---------------- McASP0 register access ---------------- */

uint32_t ev3_pru_mcasp_read(uint32_t reg) {
    return *(volatile uint32_t*)(_mmio_base + EV3_MCASP0_PHYS + reg);
}

void ev3_pru_mcasp_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(_mmio_base + EV3_MCASP0_PHYS + reg) = val;
}

/* ---------------- shared-RAM access ----------------
 * off is relative to EV3_SHARED_RAM_PHYS. _shared_ram_va is Device/uncached so
 * the ARM and PRU0 see each other's writes with no cache maintenance. */

void ev3_pru_shared_write(uint32_t off, const void* data, uint32_t bytes) {
    volatile uint8_t* dst = (volatile uint8_t*)(_shared_ram_va + off);
    const uint8_t* src = (const uint8_t*)data;
    uint32_t i;
    for (i = 0; i < bytes; i++)
        dst[i] = src[i];
}

void ev3_pru_shared_read(uint32_t off, void* data, uint32_t bytes) {
    volatile uint8_t* src = (volatile uint8_t*)(_shared_ram_va + off);
    uint8_t* dst = (uint8_t*)data;
    uint32_t i;
    for (i = 0; i < bytes; i++)
        dst[i] = src[i];
}
