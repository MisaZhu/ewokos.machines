#ifndef UC_PANEL_H
#define UC_PANEL_H

#include <stdint.h>

/*
 * ClockworkPi CM4 MIPI DSI panels.  The same fbd binary drives both:
 *
 *   cwu50  (uConsole 5")     720x1280, JD9365   — the default
 *   cwd686 (DevTerm 6.86")   480x1280, ICNL9707
 *
 * Both are 4-lane RGB888 on DSI1 with the identical reset wiring
 * (GPIO 8, active low, same pulse shape per the two DT overlays) and
 * the same OCP8178 backlight on GPIO 9.  They differ only in the DCS
 * init table, resolution/timings and HS bit clock, so panel selection
 * is a runtime switch keyed off the configured width: libfbd passes
 * /etc/framebuffer.json's width into init(), and width 480 selects
 * cwd686.  Native scan is portrait; the rotation=90 both overlays
 * declare is a compositor concern (json "rotate").
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
 * Pick the panel from the configured framebuffer width (480 → cwd686,
 * anything else → cwu50) and read the current selection back.  The
 * selection defaults to cwu50 if uc_panel_select() is never called.
 */
void uc_panel_select(uint32_t width);
const uc_panel_mode_t* uc_panel_mode(void);

void uc_panel_probe(void);
void uc_panel_reset(void);
void uc_panel_reset_inverted(void);

#endif
