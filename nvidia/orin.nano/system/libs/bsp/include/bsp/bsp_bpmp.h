#ifndef ORIN_BSP_BPMP_H
#define ORIN_BSP_BPMP_H

#include <stdint.h>

/*
 * Tegra234 BPMP IVC client (polled).  Talks to BPMP firmware over HSP_TOP0
 * doorbells + a shared-SRAM IVC ring to control clocks, resets, powergates
 * and UPHY.  Used by the cold-own PCIe RC bring-up.  See include/orin_net.h
 * for the address map and MRQ codes (upstream bpmp-abi.h / ivc.c).
 */

int bsp_bpmp_init(void);
int bsp_bpmp_probe(void);
int bsp_bpmp_ring_test(void);
int bsp_bpmp_dump(void);

int bsp_bpmp_clk_enable(uint32_t clk_id);
int bsp_bpmp_clk_set_rate(uint32_t clk_id, uint64_t rate);
int bsp_bpmp_reset_assert(uint32_t reset_id);
int bsp_bpmp_reset_deassert(uint32_t reset_id);
int bsp_bpmp_pg_on(uint32_t pg_id);
int bsp_bpmp_pg_off(uint32_t pg_id);
int bsp_bpmp_uphy_pll_init(uint32_t controller);
int bsp_bpmp_uphy_ctrl_state(uint32_t controller, uint32_t enable);

#endif