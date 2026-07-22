#ifndef UC_CWD686_H
#define UC_CWD686_H

#include <stdint.h>

/*
 * cwd686 (ClockworkPi DevTerm 6.86") panel bring-up.
 *
 * Runs the ICNL9707 vendor init table verbatim from the ClockworkPi
 * kernel driver `drivers/gpu/drm/panel/panel-cwd686.c`, framed exactly
 * like cwd686_prepare(): TE-on first, then the table, then SLPOUT +
 * DSPON with their 120/20 ms settles.
 *
 * Unlike cwu50 (all short writes) most entries here are DCS LONG
 * writes up to 39 bytes, so uc_dsi_dcs_write() must support payloads
 * beyond the 16-byte command FIFO (cmd FIFO residue + pixel FIFO).
 *
 * Returns the number of DCS commands that failed the TXPKT1_DONE
 * timeout; 0 means the whole table went out clean.
 */
int uc_cwd686_init(void);

/*
 * cwd686 default mode (see panel-cwd686.c :: default_mode).
 *  clock (kHz)   htotal   vtotal
 *   54465          694      1308
 */
#define UC_CWD686_H_ACTIVE      480U
#define UC_CWD686_H_FP          150U
#define UC_CWD686_H_SW          24U
#define UC_CWD686_H_BP          40U

#define UC_CWD686_V_ACTIVE      1280U
#define UC_CWD686_V_FP          12U
#define UC_CWD686_V_SW          6U
#define UC_CWD686_V_BP          10U

/*
 * 4 lanes, RGB888: phy_clock = pixel_clock * (24/4) = 326.79 MHz.
 * PLLD's 3 GHz VCO rounds this to divider 9 => 333.33 MHz HS
 * (~55.5 MHz pixel, ~61 Hz refresh) -- same faster-than-requested
 * rounding vc4_dsi_encoder_mode_fixup() performs.
 */
#define UC_CWD686_HS_CLOCK_HZ   326790000U

#endif
