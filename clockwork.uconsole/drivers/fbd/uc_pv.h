#ifndef UC_PV_H
#define UC_PV_H

#include <stdint.h>

/*
 * PixelValve1 — the DSI1 encoder gets pixel data from PV1's FIFO.
 * BCM2711 PV1 is at 0xfe207000 => mmio + 0x207000.
 */
#define UC_PV1_OFFSET   0x00207000U

/*
 * Split like upstream vc4:
 *   uc_pv_configure()    == vc4_crtc_config_pv() — programs all PV regs
 *                          EXCEPT PV_CONTROL_EN and PV_VCONTROL_VIDEN.
 *   uc_pv_enable()       == CRTC_WRITE(PV_CONTROL, ... | PV_CONTROL_EN).
 *   uc_pv_video_enable() == CRTC_WRITE(PV_V_CONTROL, ... | PV_VCONTROL_VIDEN).
 *
 * Correct sequence per vc4_crtc_atomic_enable:
 *   hvs_bringup -> pv_configure -> pv_enable ->
 *   dsi DISP0_ENABLE (uc_dsi_video_mode) -> pv_video_enable.
 */
int uc_pv_configure(void);
int uc_pv_enable(void);
int uc_pv_video_enable(void);

#endif
