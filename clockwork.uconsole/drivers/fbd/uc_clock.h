#ifndef UC_CLOCK_H
#define UC_CLOCK_H

#include <stdint.h>

/*
 * CPRMAN (clock/PLL) and A2W (analog-to-wrapper) register offsets for the
 * BCM2711. Everything here is relative to the CPRMAN base at MMIO+0x101000
 * — that same 8 KB window covers both the CM_* registers (0x000..0x1d4) and
 * the A2W_PLL* registers (0x1010..0x1e60).
 *
 * Every write to a CPRMAN register must OR in CM_PASSWORD in the top byte
 * or the block silently drops the write.
 *
 * Phase 2 only reads.  Phase 3 will do PLLD_DSI1 + CM_DSI1ECTL/PCTL bring-up.
 */

/* CPRMAN base offset from _mmio_base. */
#define UC_CPRMAN_OFFSET        0x101000U

/* Every CM_* / A2W_* write must be ORed with this magic in bits 31..24. */
#define UC_CM_PASSWORD          0x5a000000U

/* CM_*CTL common bits (see clk-bcm2835.c). */
#define UC_CM_ENABLE            (1U << 4)
#define UC_CM_KILL              (1U << 5)
#define UC_CM_GATE              (1U << 6)
#define UC_CM_BUSY              (1U << 7)
#define UC_CM_BUSYD             (1U << 8)
#define UC_CM_FRAC              (1U << 9)
#define UC_CM_SRC_MASK          0xfU
#define UC_CM_SRC_OSC           1U
#define UC_CM_SRC_PLLD_PER      6U
/*
 * DSI1 CM parent mux (bcm2835_clock_dsi1_parents in clk-bcm2835.c).
 *   0=gnd 1=xosc ... 8=dsi1_byte 9=dsi1_byte_inv.
 * DSI1P must source dsi1_byte or the pixel clock feeding PV1 stays gnd.
 */
#define UC_CM_SRC_DSI1_BYTE     8U

/*
 * CM_*DIV register field format is 12.12 internally, but each clock
 * registers only int_bits + frac_bits at bit 0.  For DSI1E:
 *   int_bits=4, frac_bits=8  → integer N encoded as (N << 8).
 * For DSI1P: int_bits=frac_bits=0  → register unused, pass-through.
 */
#define UC_CM_DIV_FRAC_DSI1E    8U

/* Selected CPRMAN CM_* offsets (from clk-bcm2835.c). */
#define UC_CM_OSCCOUNT          0x100U
#define UC_CM_PLLD              0x10cU
#define UC_CM_LOCK              0x114U
#define UC_CM_DSI1ECTL          0x158U   /* DSI1 escape-clock control */
#define UC_CM_DSI1EDIV          0x15cU
#define UC_CM_DSI1PCTL          0x160U   /* DSI1 pixel-clock control  */
#define UC_CM_DSI1PDIV          0x164U

/* CM_PLLD load/hold flags. */
#define UC_CM_PLLD_HOLDDSI1     (1U << 3)
#define UC_CM_PLLD_LOADDSI1     (1U << 2)

/* CM_LOCK FLOCK bits. */
#define UC_CM_LOCK_FLOCKD       (1U << 11)

/* A2W PLLD wrappers (all relative to CPRMAN base). */
#define UC_A2W_PLLD_CTRL        0x1140U
#define UC_A2W_PLLD_ANA0        0x1050U
#define UC_A2W_XOSC_CTRL        0x1190U
#define UC_A2W_PLLD_FRAC        0x1240U
#define UC_A2W_PLLD_DSI0        0x1340U
#define UC_A2W_PLLD_CORE        0x1440U
#define UC_A2W_PLLD_PER         0x1540U
#define UC_A2W_PLLD_DSI1        0x1640U

/* A2W_PLL_CTRL bits. */
#define UC_A2W_PLL_CTRL_PRST_DISABLE    (1U << 17)
#define UC_A2W_PLL_CTRL_PWRDN           (1U << 16)
#define UC_A2W_PLL_CTRL_PDIV_SHIFT      12
#define UC_A2W_PLL_CTRL_PDIV_MASK       0x00007000U
#define UC_A2W_PLL_CTRL_NDIV_MASK       0x000003ffU

/* A2W PLL channel divider register bits. */
#define UC_A2W_PLL_CHANNEL_DISABLE      (1U << 8)
#define UC_A2W_PLL_DIV_MASK             0xffU

/* A2W_XOSC_CTRL bits — need PLLD_ENABLE before PLLD locks. */
#define UC_A2W_XOSC_CTRL_PLLD_ENABLE    (1U << 5)

void     uc_clock_init(void);

/* Raw read/write helpers.  Writes automatically OR in UC_CM_PASSWORD. */
uint32_t uc_cprman_read(uint32_t off);
void     uc_cprman_write(uint32_t off, uint32_t val);

/* Slog every register the DSI1 bring-up in Phase 3 will touch. */
void     uc_clock_dump(void);

/*
 * Bring up the CPRMAN clocks feeding DSI1.  Reads PLLD's actual VCO
 * frequency out of A2W_PLLD_CTRL/FRAC and computes the A2W_PLLD_DSI1
 * integer divider that gets closest to the target HS bit rate. Sets
 * up CM_DSI1ECTL (escape) and CM_DSI1PCTL (pixel) as well. Returns 0
 * on success.
 */
int      uc_clock_bringup_dsi1(uint32_t target_hs_hz);

#endif
