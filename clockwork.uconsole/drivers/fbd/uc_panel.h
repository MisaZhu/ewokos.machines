#ifndef UC_PANEL_H
#define UC_PANEL_H

#include <stdint.h>

/*
 * ClockworkPi uConsole 5" MIPI DSI panel ("cw,cwu50"). Native scan
 * resolution is 720x1280 in portrait; the DT overlay applies rotation=90
 * downstream, which is a compositor concern rather than something the
 * panel itself understands.
 */
#define UC_PANEL_RESET_GPIO   8
#define UC_PANEL_WIDTH       720U
#define UC_PANEL_HEIGHT     1280U
#define UC_PANEL_BPP          32U

/*
 * Phase 1 API — reset only. Phase 4 will add uc_panel_send_init() to
 * run the ~180-entry cwu50 DCS sequence once DSI is up.
 */
void uc_panel_probe(void);
void uc_panel_reset(void);
void uc_panel_reset_inverted(void);

#endif
