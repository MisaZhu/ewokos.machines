#include <kernel/smp/ipi.h>
#include <kernel/core.h>
#include <kernel/system.h>
#include "x86_machine_smp.h"

void ipi_enable(uint32_t core_id) {
	(void)core_id;
}

void x86_lapic_init(void) {
	uint64_t apic_base = x86_rdmsr(IA32_APIC_BASE_MSR);

	if ((apic_base & IA32_APIC_BASE_ENABLE) == 0) {
		x86_wrmsr(IA32_APIC_BASE_MSR, apic_base | IA32_APIC_BASE_ENABLE);
	}
	x86_lapic_write(LAPIC_SVR_REG, LAPIC_SVR_ENABLE | X86_VECTOR_SPURIOUS);
	x86_lapic_write(LAPIC_EOI_REG, 0);
}

void x86_start_ap(uint32_t apic_id) {
	uint32_t vector = X86_AP_TRAMPOLINE_PADDR >> 12;

	x86_lapic_send_ipi_raw(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL |
			LAPIC_ICR_ASSERT | LAPIC_ICR_TRIGGER);
	_delay(10000000);
	x86_lapic_send_ipi_raw(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL);
	_delay(1000000);

	x86_lapic_send_ipi_raw(apic_id, LAPIC_ICR_STARTUP | vector);
	_delay(1000000);
	x86_lapic_send_ipi_raw(apic_id, LAPIC_ICR_STARTUP | vector);
	_delay(1000000);
}

void ipi_send(uint32_t core_id) {
	uint32_t apic_id;

	if (core_id == get_core_id()) {
		return;
	}
	apic_id = x86_core_id_to_apic_id(core_id);
	x86_lapic_send_ipi_raw(apic_id, X86_VECTOR_IPI);
}

void ipi_clear(uint32_t core_id) {
	(void)core_id;
}
