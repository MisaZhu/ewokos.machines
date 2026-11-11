#ifndef BSP_SDCC2_H
#define BSP_SDCC2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * User-space SDCC2 backend for the Q8B.
 *
 * sdcc2_init() maps every window it needs (GCC, SDCC2, TLMM, RPMh, Command DB)
 * and runs the complete probe: clocks, pin mux, PMIC rails, card
 * identification and the 25MHz/multi-block test.  It depends on no kernel SD
 * state or platform_data.
 */
int32_t sdcc2_init(void);
/* Re-map the SDCC2 windows in a forked child without re-probing the card. */
int32_t sdcc2_remap(void);
/* Adopt a card already identified by another process (no power cycle). */
void sdcc2_inherit(uint32_t high_capacity, uint32_t multi_ready,
        uint64_t sectors);
void sdcc2_get_state(uint32_t* high_capacity, uint32_t* multi_ready,
        uint64_t* sectors);
int32_t sdcc2_ready(void);
uint64_t sdcc2_capacity_sectors(void);
int32_t sdcc2_read_blocks(int32_t sector, void* buf, uint32_t count);
/* Test hook: allow CMD18 multi-block reads for the requested call. */
int32_t sdcc2_read_blocks_test(int32_t sector, void* buf, uint32_t count,
        uint32_t allow_multi);
int32_t sdcc2_write_blocks(int32_t sector, const void* buf, uint32_t count);
/* Test hook: allow ADMA2 multi-block CMD25 writes. */
int32_t sdcc2_write_blocks_test(int32_t sector, const void* buf,
        uint32_t count, uint32_t allow_multi);
int32_t sdcc2_read_sector(int32_t sector, void* buf);
int32_t sdcc2_write_sector(int32_t sector, const void* buf);

#ifdef __cplusplus
}
#endif

#endif
