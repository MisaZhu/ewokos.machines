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
int bsp_bpmp_clk_disable(uint32_t clk_id);
int bsp_bpmp_clk_set_rate(uint32_t clk_id, uint64_t rate);
int bsp_bpmp_clk_set_parent(uint32_t clk_id, uint32_t parent_id);
int bsp_bpmp_clk_get_rate(uint32_t clk_id, uint64_t* rate);
int bsp_bpmp_clk_round_rate(uint32_t clk_id, uint64_t rate, uint64_t* got);
int bsp_bpmp_clk_is_enabled(uint32_t clk_id, int32_t* state);
int bsp_bpmp_clk_get_parent(uint32_t clk_id, uint32_t* parent_id);
/* Hold (1) or drop (0) the top memory-clock rate as the GPU client's floor in
 * the BPMP bandwidth manager.  Without a floor the BPMP follows bus activity
 * and needs about half a second of load to leave the idle rate. */
int bsp_bpmp_gpu_memclk_hold(int hold);

/* CMD_CLK_GET_ALL_INFO response (bpmp-abi.h cmd_clk_get_all_info_response). */
typedef struct {
    uint32_t flags;
    uint32_t parent;
    uint32_t parents[16];
    uint8_t num_parents;
    uint8_t name[40];
} __attribute__((packed)) bsp_bpmp_clk_info_t;
int bsp_bpmp_clk_get_all_info(uint32_t clk_id, bsp_bpmp_clk_info_t* info);
/* BPMP in-band debugfs (MRQ_DEBUG): whole-file read. */
int bsp_bpmp_debug_read(const char* name, char* buf, uint32_t cap, uint32_t* got);
int bsp_bpmp_reset_assert(uint32_t reset_id);
int bsp_bpmp_reset_deassert(uint32_t reset_id);
int bsp_bpmp_pg_on(uint32_t pg_id);
int bsp_bpmp_pg_off(uint32_t pg_id);
int bsp_bpmp_pg_get_state(uint32_t pg_id, uint32_t* state);
int bsp_bpmp_uphy_pll_init(uint32_t controller);
int bsp_bpmp_uphy_ctrl_state(uint32_t controller, uint32_t enable);

int bsp_bpmp_thermal_num_zones(uint32_t* num);
int bsp_bpmp_thermal_get_temp(uint32_t zone, int32_t* temp_mc);
int bsp_bpmp_thermal_get_thermtrip(uint32_t zone, int32_t* temp_mc);

#endif