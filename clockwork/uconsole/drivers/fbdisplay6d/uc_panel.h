#ifndef UC_PANEL_H
#define UC_PANEL_H

#include <stdint.h>

/*
 * ClockworkPi uConsole panel wiring, matching the local
 * `clockworkpi-uconsole.dtbo` overlay:
 *
 *   panel@0 compatible = "cw,cwu50"
 *   reset-gpio         = <&gpio 8 GPIO_ACTIVE_LOW>
 *   backlight          = <&backlight>
 *   rotation           = <90>
 *
 * This fb6d build therefore drives only the 720x1280 cwu50/JD9365
 * panel. Native scan is portrait; the overlay's rotation=90 remains a
 * compositor concern (json "rotate"), not a DSI timing change.
 */
#define UC_PANEL_RESET_GPIO   8
#define UC_PANEL_WIDTH       720U
#define UC_PANEL_HEIGHT     1280U
#define UC_PANEL_BPP          32U

/*
 * Runtime description of the selected panel.  All fields mirror the
 * DRM display mode of the matching panel-*.c driver.
 */
typedef struct {
	const char* name;
	uint32_t width;
	uint32_t height;
	uint32_t hfp, hsw, hbp;
	uint32_t vfp, vsw, vbp;
	uint32_t hs_clock_hz;      /* pixel clock * 24bpp / 4 lanes */
	int (*init_table)(void);   /* vendor DCS init sequence */
} uc_panel_mode_t;

/*
 * Shared call sites still pass the configured width in, but for fb6d
 * the DT overlay fixes the hardware to cwu50 so the selector always
 * resolves to that mode.
 */
void uc_panel_select(uint32_t width);
const uc_panel_mode_t* uc_panel_mode(void);

void uc_panel_probe(void);
void uc_panel_reset(void);
void uc_panel_reset_inverted(void);

#endif
