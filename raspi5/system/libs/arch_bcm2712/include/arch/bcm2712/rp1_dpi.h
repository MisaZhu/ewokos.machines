#ifndef BCM2712_RP1_DPI_H
#define BCM2712_RP1_DPI_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>
#include <sysinfo.h>

/*
 * RP1 DPI (parallel display) output on BCM2712 / Raspberry Pi 5.
 *
 * On Pi5 the parallel interface does not live in the main SoC: the RP1
 * southbridge contains a DPI-DMA block that generates the display timing
 * and DMAs the framebuffer straight out of host RAM, plus a video PLL for
 * the pixel clock. This API brings that path up and hands back a standard
 * fbinfo_t, in the same shape bcm2712_native_hdmi_init_mode() does.
 */

typedef struct {
	uint32_t pixel_clock_hz; /* 0 = derive CVT-RB @60Hz timings */
	uint32_t hfp;
	uint32_t hsync;
	uint32_t hbp;
	uint32_t vfp;
	uint32_t vsync;
	uint32_t vbp;
	uint8_t  hsync_pos;      /* 1 = active high (default) */
	uint8_t  vsync_pos;      /* 1 = active high (default) */
	/*
	 * Pad wiring variant (rp1.dtsi "dpi mode"):
	 *   7 = 24-bit: D0..D23 on GPIO4..27 (default)
	 *   6 = DPI666 18-bit: B0..B5 on GPIO4..9, G0..G5 on GPIO12..17,
	 *       R0..R5 on GPIO20..25 (GPIO10/11/18/19 left alone)
	 */
	uint8_t  mode;
	int8_t   bl_pin;         /* backlight GPIO driven high; -1 = leave alone */
} bcm2712_dpi_timing_t;

/*
 * Bring up RP1 DPI at w x h x dep. When timing is NULL or its
 * pixel_clock_hz is 0, VESA CVT reduced blanking timings are generated for
 * 60Hz; DPI panels without such constraints can pass explicit values.
 * Returns 0 on success and fills *info.
 */
int bcm2712_rp1_dpi_init(const sys_info_t *sysinfo,
		uint32_t w, uint32_t h, uint32_t dep,
		const bcm2712_dpi_timing_t *timing, fbinfo_t *info);

/*
 * Health check for a running DPI pipe: 0 = scanning out, 1 = engine had
 * stopped and was restarted (state was logged), -1 = DPI not initialized.
 */
int bcm2712_rp1_dpi_check(void);

#endif
