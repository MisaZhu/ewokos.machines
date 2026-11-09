static int gpi_map(uintptr_t va, uintptr_t pa, unsigned size);
static uint32_t gpi_rd(uintptr_t base, unsigned off);
static void gpi_wr(uintptr_t base, unsigned off, uint32_t value);
static void gpi_delay(unsigned us);
