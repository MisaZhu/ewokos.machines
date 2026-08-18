#ifndef BCM2712_RP1_DSI_H
#define BCM2712_RP1_DSI_H

#include <stdint.h>

/*
 * RP1 MIPI DSI1 (display connector) output for BCM2712 / Raspberry Pi 5.
 *
 * On Pi5 the DSI host does not live in the main SoC: the RP1 southbridge
 * contains a MIPI1 block (rp1.dtsi rp1_dsi1) made of three register banks
 *   0x130000  DSI DMA ("ArgonDPI", same engine as rp1_dpi) scanning the
 *             framebuffer out of host RAM and feeding it to the host as an
 *             internal DPI stream,
 *   0x134000  Synopsys DWC MIPI DSI host with an SNPS D-PHY behind it,
 *   0x138000  RPI_MIPICFG block selecting DSI direction on the shared PHY.
 * Register programming follows raspberrypi/linux drivers/gpu/drm/rp1/
 * rp1-dsi/ (rp1_dsi_dsi.c + rp1_dsi_dma.c). The panel hangs off the MIPI1
 * FPC connector; its control I2C is RP1 i2c4 on GPIO40/41 (Pi5 board dts
 * i2c_csi_dsi alias). The DPHY reference clock is the 50MHz xosc.
 *
 * The framebuffer is read by RP1 over PCIe, so its DMA address is the host
 * physical address plus the RC_BAR2 inbound window offset, exactly like
 * bcm2712_rp1_dpi_init().
 */

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t hfp;
	uint32_t hsw;
	uint32_t hbp;
	uint32_t vfp;
	uint32_t vsw;
	uint32_t vbp;
	uint32_t pixel_clock_hz;
	uint32_t lanes;            /* 1..4; Waveshare panels use 2 (or 4) */
	uint32_t continuous_clock; /* 0 = HS clock returns to LP-11 in blanking */
} bcm2712_dsi_mode_t;

/*
 * Bring up clocks, D-PHY and the SNPS DSI host for the given mode. On
 * success the lanes sit at LP-11 (STOP state) with the host in command
 * mode — the moment to run the panel's I2C bring-up before video starts.
 * Returns 0 on success, a negative stage code on failure:
 *   -1 windows/RP1      -2 cfg clock        -3 DPHY PLL (no div found)
 *   -4 PLL lock timeout -5 lanes not stopped
 */
int bcm2712_rp1_dsi_init(const bcm2712_dsi_mode_t *mode);

/*
 * Configure the DSI DMA engine for a w x h x dep (16 or 32) framebuffer at
 * bus_addr (host physical + 0x1000000000), arm it, and switch the host to
 * video mode. dep 32 scans XRGB8888, dep 16 scans RGB565; on the wire the
 * panel always receives RGB888. Returns 0 on success.
 */
int bcm2712_rp1_dsi_video_start(uint64_t bus_addr, uint32_t stride,
		uint32_t w, uint32_t h, uint32_t dep);

/*
 * Health check for a running DSI pipe, same contract as
 * bcm2712_rp1_dpi_check(): 0 = scanning out, 1 = engine had stopped and was
 * restarted (state was logged), -1 = DSI not initialized.
 */
int bcm2712_rp1_dsi_check(void);

/* Register snapshot of all three MIPI1 banks for bring-up diagnosis. */
void bcm2712_rp1_dsi_dump(void);

#endif
