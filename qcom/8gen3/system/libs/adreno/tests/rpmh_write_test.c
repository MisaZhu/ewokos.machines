/* Compile the actual writer with only its MMIO and OS services replaced. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MMIO_H
static uint32_t test_read(uint64_t address);
static void test_write(uint64_t address, uint32_t value);
#define get32(address) test_read(address)
#define put32(address, value) test_write(address, value)
#include "../src/adreno_rpmh.c"

static uint32_t registers[0x10000 / 4];
static unsigned polls, irq_after, writes, clear_count;
static int triggered, stuck_irq, stuck_finish;
static uint32_t delivered_irq;
static uint32_t test_read(uint64_t address) {
    assert(address >= RSC_VA && address < RSC_VA + 0x10000);
    unsigned off = (unsigned)(address - RSC_VA);
    /* Old base+4 contains unrelated nonzero configuration, never an IRQ. */
    if (off == 4) return 0x4010100;
    if (off == 0xd04 && triggered && ++polls >= irq_after)
        registers[off / 4] |= delivered_irq;
    if (off == 0xd3c) return 1u << 16; /* retained COMPL from poll zero */
    return registers[off / 4];
}
static void test_write(uint64_t address, uint32_t value) {
    assert(address >= RSC_VA && address < RSC_VA + 0x10000);
    unsigned off = (unsigned)(address - RSC_VA);
    assert(off >= 0xd00); /* Never modify RSC config words 0/8. */
    writes++;
    if (off == 0xd08) {
        clear_count++;
        if (!stuck_irq) registers[0xd04 / 4] &= ~value;
        return;
    }
    if (off == 0xd14 && value == 0 && triggered && stuck_finish) return;
    registers[off / 4] = value;
    if (off == 0xd14 && (value & (1u << 24))) triggered = 1;
}
int proc_nsleep_precise(uint64_t ns) { (void)ns; return 0; }
void klog(const char *format, ...) { (void)format; }
ewokos_addr_t syscall3_raw(int op, ewokos_addr_t a, ewokos_addr_t b,
        ewokos_addr_t c) { (void)op; (void)b; (void)c; return a; }
static void reset_bus(void) {
    memset(registers, 0, sizeof(registers));
    polls = writes = clear_count = 0;
    triggered = stuck_irq = stuck_finish = rpmh_failed = 0;
    rpmh_compl_without_irq = 0;
    irq_after = 7; delivered_irq = 1;
}
int main(void) {
    reset_bus(); registers[0xd04 / 4] = 1; /* previous IRQ must clear first */
    assert(rpmh_write(0x50054, 0x60003fff) == 0);
    assert(polls == 7 && rpmh_compl_without_irq == 6 && clear_count == 2);
    assert(registers[0xd34 / 4] == 0x50054);
    assert(registers[0xd38 / 4] == 0x60003fff);
    assert(registers[0xd14 / 4] == 0 && registers[0xd1c / 4] == 0);

    reset_bus(); irq_after = UINT32_MAX;
    assert(rpmh_write(0x50054, 0x60003fff) != 0);
    assert(polls == 1000001 && clear_count == 1 && rpmh_failed);
    assert(registers[0xd14 / 4] & (1u << 24)); /* no false cancellation */
    unsigned before = writes;
    assert(rpmh_write(0x5001c, 0x60003fff) != 0 && writes == before);

    reset_bus(); delivered_irq = 2;
    assert(rpmh_write(0x50054, 0x60003fff) != 0 && clear_count == 1);

    reset_bus(); stuck_irq = 1; registers[0xd04 / 4] = 1;
    assert(rpmh_write(0x50054, 0x60003fff) != 0 && !triggered);

    reset_bus(); stuck_finish = 1;
    assert(rpmh_write(0x50054, 0x60003fff) != 0 && rpmh_failed);

    reset_bus(); registers[0xd14 / 4] = 1u << 16;
    assert(rpmh_write(0x50054, 0x60003fff) != 0 && writes == 0);
    puts("PASS: Adreno production RPMh fresh IRQ, stale COMPL, other TCS, stuck clear/cleanup, timeout quarantine and active TCS refusal");
    return 0;
}
