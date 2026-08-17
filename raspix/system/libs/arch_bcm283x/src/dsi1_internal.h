#ifndef __DSI1_INTERNAL_H__
#define __DSI1_INTERNAL_H__

#include <stdint.h>

#include <arch/bcm283x/dsi1.h>

/*
 * Shared internals of the bcm283x DSI pipeline.  Block offsets from
 * _mmio_base are identical on BCM2835/2837 (gen4) and BCM2711 (gen5).
 *
 * Two DSI ports exist.  The 15-pin DISPLAY connector is wired to DSI1
 * on every consumer Pi (Pi3 included); DSI0 pads only exist on Compute
 * Modules.  Every block accessor below routes through the port
 * selected with dsi1_set_port().  NOTE gen4 DSI1 register writes are
 * silently dropped by its broken AXI slave and must go through the
 * DMA engine (vc4_dsi.c) — dsi1_dsi_write() handles that.
 */
#define DSI1_CPRMAN_OFFSET      0x101000U
#define DSI1_HVS_OFFSET         0x00400000U
#define DSI1_STC_CLO_OFFSET     0x00003004U

/* Per-port block offsets. */
#define DSI0_DSI_OFFSET         0x209000U
#define DSI1_DSI_OFFSET         0x700000U
#define DSI0_PV_OFFSET          0x206000U   /* PixelValve0 feeds DSI0 */
#define DSI1_PV_OFFSET          0x207000U   /* PixelValve1 feeds DSI1 */

/* Every CM_xxx / A2W_xxx write must be ORed with this magic in bits 31..24. */
#define DSI1_CM_PASSWORD        0x5a000000U

/* XOSC frequencies: 19.2 MHz on gen4, 54 MHz on gen5. */
#define DSI1_XOSC_GEN4_HZ       19200000U
#define DSI1_XOSC_GEN5_HZ       54000000U

/* Escape clock target — vc4_dsi.c does clk_set_rate(escape, 100 MHz)
 * and every escape-domain timing constant assumes 10 ns per tick. */
#define DSI1_ESC_CLOCK_HZ       100000000U

/* ---------- shared accessors (dsi1_common.c) ---------- */

uint32_t dsi1_xosc_hz(void);
uint32_t dsi1_micros(void);

/* Selected DSI port (0 or 1); defaults to 1 until set/probed. */
int  dsi1_port(void);
void dsi1_set_port(int port);

/* Raw block accessors; reads return 0 and writes are dropped before
 * _mmio_base is mapped.  DSI/PV accessors route through dsi1_port().
 * CPRMAN writes OR in DSI1_CM_PASSWORD. */
uint32_t dsi1_cprman_read(uint32_t off);
void     dsi1_cprman_write(uint32_t off, uint32_t val);
uint32_t dsi1_dsi_read(uint32_t off);
void     dsi1_dsi_write(uint32_t off, uint32_t val);
uint32_t dsi1_dsi_read_port(int port, uint32_t off);
uint32_t dsi1_pv_read_port(int port, uint32_t off);
uint32_t dsi1_hvs_read(uint32_t off);
void     dsi1_hvs_write(uint32_t off, uint32_t val);
void     dsi1_hvs_dump_live(void);
uint32_t dsi1_pv_read(uint32_t off);
void     dsi1_pv_write(uint32_t off, uint32_t val);

/* gen4-DSI1 register-write DMA workaround failure count. */
uint32_t dsi1_reg_dma_errors(void);

#endif
