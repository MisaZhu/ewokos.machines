#ifndef __EV3_PRU_H__
#define __EV3_PRU_H__

#include <stdint.h>
#include <ewoksys/ewokdef.h>

/*
 * AM1808 (DA850) PRUSS low-level HAL for the EV3.
 *
 * EV3 input ports 3 and 4 have no hardware UART (all three 16550s are spent:
 * UART1=port1, UART0=port2, UART2=Bluetooth). Like LEGO/ev3dev/PyBricks we
 * drive them with the TI "PRU SUART Emulation" firmware running on PRU0, which
 * bit-bangs two McASP-serialiser-backed soft-UARTs. This file is only the
 * register/RAM plumbing; the SUART protocol lives in pru_uart.c.
 *
 * Everything below PRUSS_BASE lives inside the kernel's 32 MB MMIO window
 * (mmio.phy_base=0, size=32 MB), so it is reachable as _mmio_base + phys with
 * no extra mapping. The ARM<->PRU shared RAM at 0x8000_0000 is OUTSIDE that
 * window and is mapped separately by ev3_pru_init() (the kernel whitelists it
 * in check_mem_map_arch(); it is mapped Device/uncached for PRU coherency).
 */

/* ---- Physical register bases ---- */
#define EV3_PRUSS_PHYS      0x01C30000u   /* PRUSS (PRU0/PRU1 + INTC)   */
#define EV3_MCASP0_PHYS     0x01D00000u   /* McASP0 (serialisers)       */
#define EV3_PSC0_PHYS       0x01C10000u   /* gates PRUSS  (LPSC 13)     */
#define EV3_PSC1_PHYS       0x01E27000u   /* gates McASP0 (LPSC 7)      */

/* ---- PRUSS internal offsets (relative to EV3_PRUSS_PHYS) ----
 * DA850 PRUSS memory map (spruh82 PRUSS chapter). Verified against the TI
 * pru.c loader: PRU0 IRAM=+0x8000, PRU1 IRAM=+0xc000, PRU0 CTRL=+0x7000,
 * PRU1 CTRL=+0x7800, PRU0 DRAM=+0x0000. INTC is the CP-INTC register file at
 * +0x4000 (same IP the kernel's own AINTC driver uses - see kernel/bsp/irq.c,
 * so the register offsets there are authoritative for this chip). */
#define EV3_PRU0_DRAM_OFF   0x0000u   /* 8 KB data RAM  */
#define EV3_PRU1_DRAM_OFF   0x2000u   /* 8 KB data RAM  */
#define EV3_PRUSS_INTC_OFF  0x4000u   /* CP-INTC regs   */
#define EV3_PRU0_CTRL_OFF   0x7000u   /* PRU0 control   */
#define EV3_PRU1_CTRL_OFF   0x7800u   /* PRU1 control   */
#define EV3_PRU0_IRAM_OFF   0x8000u   /* 8 KB instr RAM */
#define EV3_PRU1_IRAM_OFF   0xC000u   /* 8 KB instr RAM */
#define EV3_PRU_DRAM_SIZE   0x2000u
#define EV3_PRU_IRAM_SIZE   0x2000u

/* ---- PRU core CONTROL register (offset 0 within a CTRL block) ----
 * GATE-0 CRITICAL. Bit positions are taken verbatim from the TI CSL for this
 * exact core (cslr_prucore.h, CSL_PRUCORE_CONTROL_*): SOFTRESET is bit 0,
 * ENABLE is bit 1 (NOT bit 0), COUNTENABLE is bit 3, SINGLESTEP bit 8, and the
 * read-only RUNSTATE is bit 15 (NOT bit 16); PCRESETVAL occupies bits 16-31.
 * TI's pru_run() releases the core with CONTROL |= COUNTENABLE|ENABLE (= 0xA)
 * and never sets SOFTRESET, so ev3_pru_run() mirrors that exactly. RUNSTATE is
 * the read-only "core running" flag polled by ev3_pru_is_running(). */
#define EV3_PRU_CTRL_SOFTRESET       (1u << 0)   /* 1 = out of reset   */
#define EV3_PRU_CTRL_ENABLE          (1u << 1)   /* PruEnable          */
#define EV3_PRU_CTRL_COUNTER_ENABLE  (1u << 3)   /* CycleCounterEn     */
#define EV3_PRU_CTRL_RUNSTATE        (1u << 15)  /* R/O: running       */
#define EV3_PRU_CTRL_RESETVAL        0u

/* ---- PSC LPSC module numbers ---- */
#define EV3_PSC_MOD_PRUSS   13   /* PSC0 module gating PRUSS  */
#define EV3_PSC_MOD_MCASP0   7   /* PSC1 module gating McASP0 */

/* ---- ARM<->PRU shared RAM (DA850 on-chip SRAM, outside MMIO window) ----
 * The SUART TX/RX FIFOs are staged here (phys 0x8000_0000); the PRU reaches it
 * through its constant table while the ARM maps it at _mmio_base + mmio.size.
 * 8 KiB covers the 4 x 512 B FIFO buffers (2 SUARTs x TX/RX). */
#define EV3_SHARED_RAM_PHYS 0x80000000u
#define EV3_SHARED_RAM_SIZE 0x00002000u

/*
 * Map the PRUSS/McASP/PSC window (via _mmio_base) and the shared RAM. Returns
 * 0 on success, -1 if the shared-RAM mapping was refused. Idempotent: safe to
 * call from every daemon; must be called before any accessor below.
 */
int ev3_pru_init(void);

/* Virtual bases (valid only after ev3_pru_init() == 0). */
ewokos_addr_t ev3_pru_base(void);        /* PRUSS registers          */
ewokos_addr_t ev3_pru_mcasp_base(void);  /* McASP0 registers         */
ewokos_addr_t ev3_pru_shared_ram(void);  /* ARM<->PRU shared RAM     */

/* Clock gating (PSC LPSC). An unclocked DA850 peripheral silently discards all
 * register writes, so both must be enabled before touching PRUSS/McASP. */
void ev3_pru_psc_enable(void);           /* PRUSS  via PSC0 LPSC 13 */
void ev3_pru_mcasp_psc_enable(void);     /* McASP0 via PSC1 LPSC 7  */

/* PRU core lifecycle (pru = 0 or 1). */
void ev3_pru_reset(int pru);             /* hold core in reset (CONTROL = 0) */
int  ev3_pru_load(int pru, const uint32_t* code, uint32_t words);
void ev3_pru_run(int pru);               /* ENABLE | COUNTER_ENABLE          */
void ev3_pru_disable(int pru);
int  ev3_pru_is_running(int pru);        /* 1 when RUNSTATE says running     */

/* PRU data-RAM access. off is absolute from the PRUSS base (PRU0 DRAM starts
 * at EV3_PRU0_DRAM_OFF = 0). Mirrors the TI pru_ram_*_data helpers the SUART
 * firmware protocol relies on (byte- and word-granular). */
void ev3_pru_ram_write(uint32_t off, const void* data, uint32_t bytes);
void ev3_pru_ram_read(uint32_t off, void* data, uint32_t bytes);
void ev3_pru_ram_write32(uint32_t off, const uint32_t* data, uint32_t words);
void ev3_pru_ram_read32(uint32_t off, uint32_t* data, uint32_t words);

/* PRUSS INTC register access (reg is relative to EV3_PRUSS_INTC_OFF). */
uint32_t ev3_pru_intc_read(uint32_t reg);
void     ev3_pru_intc_write(uint32_t reg, uint32_t val);

/* McASP0 register access (reg is relative to EV3_MCASP0_PHYS). */
uint32_t ev3_pru_mcasp_read(uint32_t reg);
void     ev3_pru_mcasp_write(uint32_t reg, uint32_t val);

/* Shared-RAM access (off relative to EV3_SHARED_RAM_PHYS). */
void     ev3_pru_shared_write(uint32_t off, const void* data, uint32_t bytes);
void     ev3_pru_shared_read(uint32_t off, void* data, uint32_t bytes);

#endif /* __EV3_PRU_H__ */
