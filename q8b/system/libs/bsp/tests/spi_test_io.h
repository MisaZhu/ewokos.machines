#ifndef Q8B_SPI_TEST_IO_H
#define Q8B_SPI_TEST_IO_H
static uint32_t hw_rd(uintptr_t base, unsigned offset);
static void hw_wr(uintptr_t base, unsigned offset, uint32_t value);
static int hw_map(uintptr_t va, uintptr_t pa, unsigned size);
static void hw_delay(unsigned us);
static uint64_t hw_time(void);
#endif
