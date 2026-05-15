#ifndef BSP_X86_MACHINE_SMP_H
#define BSP_X86_MACHINE_SMP_H

#include <stdint.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <x86_smp.h>
#include "arch.h"

#define IA32_APIC_BASE_MSR        0x1b
#define IA32_APIC_BASE_ENABLE     0x800

#define LAPIC_ID_REG              0x020
#define LAPIC_EOI_REG             0x0b0
#define LAPIC_SVR_REG             0x0f0
#define LAPIC_ESR_REG             0x280
#define LAPIC_ICR_LOW_REG         0x300
#define LAPIC_ICR_HIGH_REG        0x310
#define LAPIC_LVT_TIMER_REG       0x320
#define LAPIC_LVT_LINT0_REG       0x350
#define LAPIC_LVT_LINT1_REG       0x360
#define LAPIC_LVT_ERROR_REG       0x370

#define LAPIC_SVR_ENABLE          0x100
#define LAPIC_LVT_MASKED          0x00010000
#define LAPIC_ICR_INIT            0x00000500
#define LAPIC_ICR_STARTUP         0x00000600
#define LAPIC_ICR_LEVEL           0x00004000
#define LAPIC_ICR_ASSERT          0x00004000
#define LAPIC_ICR_TRIGGER         0x00008000
#define LAPIC_ICR_DELIVERY_BUSY   0x00001000
#define LAPIC_ICR_ALL_EX_SELF     0x000c0000

#define X86_AP_TRAMPOLINE_VADDR   (INTERRUPT_VECTOR_BASE - PAGE_SIZE)

extern uint8_t x86_ap_trampoline_start[];
extern uint8_t x86_ap_trampoline_end[];
extern uint8_t x86_ap_trampoline_boot_pml4[];
extern uint8_t x86_ap_trampoline_kernel_vm[];
extern uint8_t x86_ap_trampoline_stack[];
extern uint8_t x86_ap_trampoline_kernel_entry[];

static inline void x86_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
		uint32_t *ecx, uint32_t *edx) {
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;

	__asm__ volatile("cpuid"
		: "=a"(a), "=b"(b), "=c"(c), "=d"(d)
		: "a"(leaf), "c"(0));

	if (eax != 0) {
		*eax = a;
	}
	if (ebx != 0) {
		*ebx = b;
	}
	if (ecx != 0) {
		*ecx = c;
	}
	if (edx != 0) {
		*edx = d;
	}
}

static inline uint64_t x86_rdmsr(uint32_t msr) {
	uint32_t lo;
	uint32_t hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void x86_wrmsr(uint32_t msr, uint64_t value) {
	uint32_t lo = (uint32_t)value;
	uint32_t hi = (uint32_t)(value >> 32);

	__asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint32_t x86_lapic_phys_base(void) {
	uint64_t base = x86_rdmsr(IA32_APIC_BASE_MSR);
	return (uint32_t)(base & 0xfffff000u);
}

static inline uintptr_t x86_lapic_vaddr(void) {
	uint32_t phys = x86_lapic_phys_base();
	return _sys_info.mmio.v_base + (phys - _sys_info.mmio.phy_base);
}

static inline uint32_t x86_lapic_read(uint32_t reg) {
	return get32(x86_lapic_vaddr() + reg);
}

static inline void x86_lapic_write(uint32_t reg, uint32_t value) {
	put32(x86_lapic_vaddr() + reg, value);
	(void)x86_lapic_read(LAPIC_ID_REG);
}

static inline int x86_lapic_wait_icr(void) {
	for (uint32_t i = 0; i < 1000000; i++) {
		if ((x86_lapic_read(LAPIC_ICR_LOW_REG) & LAPIC_ICR_DELIVERY_BUSY) == 0) {
			return 0;
		}
	}
	return -1;
}

static inline void x86_lapic_send_ipi_raw(uint32_t apic_id, uint32_t icr_low) {
	if (x86_lapic_wait_icr() != 0) {
		return;
	}
	x86_lapic_write(LAPIC_ICR_HIGH_REG, apic_id << 24);
	x86_lapic_write(LAPIC_ICR_LOW_REG, icr_low);
	(void)x86_lapic_wait_icr();
}

void x86_lapic_init(void);
void x86_irq_percpu_init(void);
void x86_start_ap(uint32_t apic_id);

#endif
