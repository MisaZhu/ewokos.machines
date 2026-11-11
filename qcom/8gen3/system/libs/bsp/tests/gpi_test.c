#include <assert.h>
#include <stdio.h>
#include <string.h>
#define Q8B_GPI_TEST
#include "../src/gpi.c"

static uint32_t mmio[0x4000 / 4];
static unsigned writes, cmd_count, fail_cmd, fail_map;
static unsigned no_irq_cmd, wrong_irq_cmd, bad_state_cmd, stuck_clear;
static int gpi_map(uintptr_t va, uintptr_t pa, unsigned size) {
    assert(va == 0x5b108000 && pa >= 0x810000 && pa <= 0x83c000);
    assert(size == 0x4000);
    return fail_map ? -1 : 0;
}
static uint32_t gpi_rd(uintptr_t base, unsigned off) {
    assert(base == 0x5b108000 && off < 0x4000);
    return mmio[off / 4];
}
static void gpi_wr(uintptr_t base, unsigned off, uint32_t value) {
    assert(base == 0x5b108000);
    writes++;
    /* Control-plane tests must never program ring contexts, start a channel,
     * enable interrupts or issue a DMA doorbell. */
    assert(off == 0x3010 || off == 0x3008 || off == 0x30a0 || off == 0x30a4);
    if (off == 0x30a0 || off == 0x30a4) {
        if (!stuck_clear) mmio[(off - 0x10) / 4] &= ~value;
        return;
    }
    unsigned op = value >> 24;
    unsigned ch = value & 255;
    assert(op == 0 || op == 10);
    assert(ch < 2);
    cmd_count++;
    if (cmd_count == fail_cmd)
        return;
    unsigned ctx = off == 0x3010 ? 0x1000 : ch * 0x80;
    /* Hardware DEALLOC keeps the last context state (Linux gpi.c), so it
     * must succeed based on its fresh command IRQ, not a synthetic zero. */
    if (op == 0) mmio[ctx / 4] = (cmd_count == bad_state_cmd ? 15U : 1U) << 20;
    if (cmd_count != no_irq_cmd) {
        unsigned irq = off == 0x3010 ? 0x3094 : 0x3090;
        mmio[irq / 4] |= 1U << (cmd_count == wrong_irq_cmd ? ch ^ 1 : ch);
    }
}
static void gpi_delay(unsigned us) { assert(us == 100); }
static void reset(void) {
    memset(mmio, 0, sizeof(mmio));
    writes = cmd_count = fail_cmd = fail_map = 0;
    no_irq_cmd = wrong_irq_cmd = bad_state_cmd = stuck_clear = 0;
}
int main(void) {
    for (unsigned n = 0; n < 12; n++) {
        reset();
        q8b_gpi_t g = {0};
        assert(q8b_gpi_acquire(&g, n) == 0);
        assert(g.event == 1 && g.channels == 3 && cmd_count == 3);
        unsigned before = writes;
        assert(q8b_gpi_acquire(&g, n) == -1 && writes == before);
        assert(q8b_gpi_release(&g) == 0);
        assert(!g.event && !g.channels && !g.poisoned && g.retired && cmd_count == 6);
        assert(mmio[0] == 1U << 20 && mmio[0x80 / 4] == 1U << 20);
        before = writes;
        assert(q8b_gpi_acquire(&g, n) == -1 && writes == before);
    }
    for (unsigned ctx = 0; ctx < 3; ctx++) {
        reset();
        mmio[(ctx == 2 ? 0x1000 : ctx * 0x80) / 4] = 2U << 20;
        q8b_gpi_t g = {0};
        assert(q8b_gpi_acquire(&g, 0) == -1 && writes == 0);
    }
    for (unsigned failed = 1; failed <= 6; failed++) {
        reset(); fail_cmd = failed;
        q8b_gpi_t g = {0};
        if (failed <= 3)
            assert(q8b_gpi_acquire(&g, 0) == -1);
        else {
            assert(q8b_gpi_acquire(&g, 0) == 0);
            assert(q8b_gpi_release(&g) == -1);
        }
        assert(g.poisoned);
        unsigned before = writes;
        assert(q8b_gpi_acquire(&g, 0) == -1);
        assert(q8b_gpi_release(&g) == -1 && before == writes);
    }
    /* A state transition without an IRQ is not an acknowledgement. Test
     * each command, including DEALLOC whose context stays allocated. */
    for (unsigned lost = 1; lost <= 6; lost++) {
        reset(); no_irq_cmd = lost;
        q8b_gpi_t g = {0};
        if (lost <= 3) assert(q8b_gpi_acquire(&g, 0) == -1);
        else {
            assert(q8b_gpi_acquire(&g, 0) == 0);
            assert(q8b_gpi_release(&g) == -1);
        }
        assert(g.poisoned && cmd_count == lost);
    }
    for (unsigned stale = 0; stale < 3; stale++) {
        reset();
        mmio[(stale == 2 ? 0x3094 : 0x3090) / 4] = 1U << (stale & 1);
        q8b_gpi_t g = {0};
        assert(q8b_gpi_acquire(&g, 0) == -1 && writes == 0);
    }
    reset(); wrong_irq_cmd = 2;
    q8b_gpi_t wrong = {0};
    assert(q8b_gpi_acquire(&wrong, 0) == -1 && wrong.poisoned);
    assert(mmio[0x3090 / 4] == 2); /* unrelated RX IRQ not consumed */
    reset(); bad_state_cmd = 1;
    q8b_gpi_t bad = {0};
    assert(q8b_gpi_acquire(&bad, 0) == -1 && bad.poisoned);
    reset(); stuck_clear = 1;
    q8b_gpi_t sticky = {0};
    assert(q8b_gpi_acquire(&sticky, 0) == -1 && sticky.poisoned);
    reset();
    q8b_gpi_t g = {0};
    assert(q8b_gpi_acquire(&g, 0) == 0);
    mmio[0] = 2U << 20;
    unsigned before = writes;
    assert(q8b_gpi_release(&g) == -1 && g.poisoned && writes == before);
    reset(); memset(&g, 0, sizeof(g));
    assert(q8b_gpi_acquire(&g, 0) == 0);
    mmio[0x3090 / 4] = 1; /* stale TX completion before DEALLOC */
    before = writes;
    assert(q8b_gpi_release(&g) == -1 && g.poisoned && writes == before);
    reset(); memset(&g, 0, sizeof(g)); fail_map = 1;
    assert(q8b_gpi_acquire(&g, 0) == -1 && writes == 0);
    assert(q8b_gpi_acquire(&g, 12) == -1 && writes == 0);
    assert(q8b_gpi_acquire(NULL, 0) == -1);
    puts("q8b GPI control tests: ok");
    return 0;
}
