#ifndef UC_CWU50_H
#define UC_CWU50_H

#include <stdint.h>

/*
 * cwu50 (ClockworkPi uConsole 5") panel bring-up.
 *
 * Mirrors the Linux `panel-cwu50.c` bring-up used by ClockworkPi's
 * newer uConsole screen support, but fb6d is now intentionally scoped
 * to the new hardware only:
 *  - send TE first
 *  - send SLPOUT and read DCS 0x04 for evidence only
 *  - always run the "future LCD" / new-panel vendor init table
 *
 * Preconditions:
 *  - uc_panel_reset() has been called (Phase 1)
 *  - uc_clock_bringup_dsi1() and uc_dsi_bringup() have run (Phase 3)
 *  - uc_dsi_dcs_write() can transmit in LP command mode
 *
 * Returns the number of DCS commands that failed the TXPKT1_DONE
 * timeout; 0 means the selected init path went out clean.
 */
int uc_cwu50_init(void);

/*
 * cwu50 default mode (see panel-cwu50.c :: default_mode).
 *  clock (kHz)   htotal   vtotal
 *   62500          803      1306
 */
#define UC_CWU50_H_ACTIVE       720U
#define UC_CWU50_H_FP           43U
#define UC_CWU50_H_SW           20U
#define UC_CWU50_H_BP           20U
#define UC_CWU50_H_TOTAL        (UC_CWU50_H_ACTIVE + UC_CWU50_H_FP + UC_CWU50_H_SW + UC_CWU50_H_BP)

#define UC_CWU50_V_ACTIVE       1280U
#define UC_CWU50_V_FP           8U
#define UC_CWU50_V_SW           2U
#define UC_CWU50_V_BP           16U
#define UC_CWU50_V_TOTAL        (UC_CWU50_V_ACTIVE + UC_CWU50_V_FP + UC_CWU50_V_SW + UC_CWU50_V_BP)

#define UC_CWU50_PIXEL_KHZ      62500U

#endif
