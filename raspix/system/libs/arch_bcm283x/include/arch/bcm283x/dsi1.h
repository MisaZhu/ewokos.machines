#ifndef __ARCH_BCM283X_DSI1_H__
#define __ARCH_BCM283X_DSI1_H__

#include <stdint.h>

/*
 * Bare-metal DSI1 display pipeline for BCM2835/BCM2837 (gen4, Pi3) and
 * BCM2711 (gen5, Pi4).  Mirrors the Linux vc4 driver's bring-up order:
 *
 *   CPRMAN clocks (PLLD_DSI1 + DSI1E + DSI1P)
 *   -> DSI1 PHY/host into LP-11            (bcm283x_dsi1_host_bringup)
 *   -> panel init (caller, I2C or DCS)
 *   -> HVS channel + dlist scan-out        (bcm283x_dsi1_hvs_bringup)
 *   -> PixelValve1 timing                  (bcm283x_dsi1_pv_configure)
 *   -> PV EN / VIDEN / DSI video mode
 *
 * All block offsets are relative to the MMIO base and identical on
 * both SoCs; the generation is detected at runtime from
 * sysinfo.mmio.phy_base (0x3f000000 = gen4, 0xfe000000 = gen5).
 */

/* VC firmware power domains (RPI_FIRMWARE_SET_DOMAIN_STATE interface).
 * The firmware domain index is the DT binding index + 1
 * (see raspberrypi-power.c).  Indices from raspberrypi-power.h. */
#define BCM283X_PWR_DOMAIN_VIDEO_SCALER   (3 + 1)   /* HVS */
#define BCM283X_PWR_DOMAIN_DSI0           (17 + 1)  /* DSI0 host + analog PHY */
#define BCM283X_PWR_DOMAIN_DSI1           (18 + 1)  /* DSI1 host + analog PHY */

/*
 * Requested panel mode.  Mirrors a DRM display mode plus the MIPI link
 * parameters.  pixel_clock_hz is the panel's nominal pixel clock; the
 * CPRMAN bring-up may stretch blanking to honour the integer-only
 * PLLD_DSI1 divider (see bcm283x_dsi1_clock_bringup).
 */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t hfp, hsw, hbp;
	uint32_t vfp, vsw, vbp;
	uint32_t pixel_clock_hz;
	uint32_t lanes;              /* 1..4 data lanes */
	int      continuous_clock;   /* 0 = non-continuous HS clock (LP-11 between HS bursts) */
} bcm283x_dsi1_mode_t;

/*
 * The mode as actually programmed after the integer-PLL fixup.  hfp may
 * have grown so the refresh rate stays at the panel's nominal value.
 */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t hfp, hsw, hbp;
	uint32_t vfp, vsw, vbp;
	uint32_t pixel_clock_hz;     /* pixel clock actually generated */
	uint32_t hs_clock_hz;        /* HS bit clock programmed into PLLD_DSI1 */
	uint32_t pix_clk_divider;    /* DSI DISP0 PIX_CLK_DIV = 24bpp / lanes */
} bcm283x_dsi1_adjusted_mode_t;

/* 1 on BCM2711 (Pi4/CM4), 0 on BCM2835/2837 (Pi3). */
int bcm283x_dsi1_is_gen5(void);

/*
 * DSI port selection.  The display connector is wired to DSI0 on some
 * boards (Pi3 family) and DSI1 on others (Pi4/CM4); the two ports also
 * differ in register layout, so the whole library routes through the
 * selected port.  probe_port() is a write-readback liveness test (a
 * powered-off port silently drops writes): 0 = alive.  set_port()
 * switches every DSI/PV accessor; port() returns the current one.
 */
int  bcm283x_dsi1_probe_port(int port);
void bcm283x_dsi1_set_port(int port);
int  bcm283x_dsi1_port(void);

/* Busy-wait helpers on the 1MHz system timer (STC CLO). */
void bcm283x_dsi1_udelay(uint32_t us);
void bcm283x_dsi1_mdelay(uint32_t ms);
uint32_t bcm283x_dsi1_millis(void);

/*
 * VC firmware power-domain helpers (property mailbox).
 * get: 1 = on, 0 = off, -1 = mailbox failure.
 * set: 0 on firmware ACK, -1 otherwise.
 * NOTE: upstream Linux never power-cycles the DSI1 domain (no dtsi
 * carries power-domains on &dsi1); only enable it when reported off.
 */
int bcm283x_dsi1_power_domain_get(uint32_t domain);
int bcm283x_dsi1_power_domain_set(uint32_t domain, int on);

/*
 * Compute the integer-PLL adjusted mode and bring up the CPRMAN clocks:
 * A2W_PLLD_DSI1 divider, CM_DSI1ECTL (escape, 100 MHz from PLLD_PER)
 * and CM_DSI1PCTL (pixel, sourced from the DSI byte clock).
 *
 * PLLD_DSI1 only has an integer divider, so the HS bit clock lands on
 * vco/round(vco/target).  Like vc4_dsi_bridge_mode_fixup() the extra
 * clock is paid back by extending hfp so the panel refresh is kept.
 * Fills *adj with the mode as actually programmed.  Returns 0 on success.
 */
int bcm283x_dsi1_clock_bringup(const bcm283x_dsi1_mode_t* mode,
		bcm283x_dsi1_adjusted_mode_t* adj);

/*
 * Measure a CPRMAN clock's real frequency with the TCNT counter
 * (bcm2835_measure_tcnt_mux).  mux values: 19 = DSI1E, 13 = DSI1P.
 * Returns Hz, or 0 on timeout / dead clock.
 */
uint32_t bcm283x_dsi1_measure_hz(uint32_t tcnt_mux);

/* ID register probe: 0 iff DSI1 reads 0x00647369 ("dsi"). */
int bcm283x_dsi1_alive(void);

/*
 * Dump the clock/PHY/lane register banks + measured DSI1E/DSI1P
 * frequencies to the console.  For the daemon's fatal bring-up path.
 */
void bcm283x_dsi1_dump(void);
void bcm283x_dsi1_dump_firmware(void);
void bcm283x_dsi1_dump_live(void);
void bcm283x_dsi1_mbox_stats(void);
void bcm283x_dsi1_firmware_handoff(void);

/*
 * Full DSI1 controller + analog PHY bring-up (vc4_dsi_encoder_enable
 * without the video-mode write).  Leaves the lanes in LP-11 STOP so
 * panel commands can go out.  bcm283x_dsi1_clock_bringup() must have
 * run first.  hs_clock_hz must be adj.hs_clock_hz from the clock
 * bring-up (it drives the PHY timing computation).
 */
int bcm283x_dsi1_host_bringup(uint32_t lanes, uint32_t hs_clock_hz,
		int continuous_clock);

/* 0 iff every enabled data lane reports LP-11 STOP in STAT. */
int bcm283x_dsi1_lanes_stopped(void);

/*
 * Send one MIPI DSI packet.  data_type is the MIPI data type byte
 * (0x05 DCS short write, 0x39 DCS long write, ...).  For short packets
 * payload carries up to 2 parameter bytes.  Returns 0 when TXPKT1_DONE
 * was observed within the timeout.
 */
int bcm283x_dsi1_cmd_write(uint8_t data_type, const uint8_t* payload,
		uint32_t len);

/*
 * Switch DISP0 into video mode with the full ENABLE write (call AFTER
 * the panel init, with PV already pushing pixels):
 *   DISP0_CTRL = PIX_CLK_DIV | PFORMAT_RGB888 | LP_STOP_PERFRAME |
 *                ST_END | ENABLE
 */
void bcm283x_dsi1_video_mode(uint32_t pix_clk_divider);

/*
 * Program the HVS display list for one full-screen unity plane and
 * enable HVS channel 1 (the channel feeding PV1 via SCALER_DISPCTRL
 * DSP3_MUX, on both gen4 and gen5).  phy_fb is the ARM physical
 * scan-out address; the bus alias (0xC0000000) is added internally.
 * pitch is the framebuffer row stride in bytes (0 = w*bpp).
 */
int bcm283x_dsi1_hvs_bringup(uint32_t phy_fb, uint32_t w, uint32_t h,
		uint32_t dep, uint32_t pitch);

/*
 * Post-start liveness probes:
 *  wait_running: 0 iff the HVS channel left INIT (RUN/EOF) within
 *                timeout_ms — the PV delivered a vstart.
 *  frames_advancing: 0 iff a frame completed within wait_ms.
 */
int bcm283x_dsi1_hvs_wait_running(uint32_t timeout_ms);
int bcm283x_dsi1_hvs_frames_advancing(uint32_t wait_ms);
/* Empirical PV->FIFO crossbar probe (gen4 DSI1 only): if PV1's
 * vstart never reaches FIFO 2, try FIFO 0 and re-target the scan-out
 * channel on success.  0 = channel switched, -1 = no vstart anywhere. */
int bcm283x_dsi1_hvs_crossbar_probe(void);

/*
 * PixelValve1 (the DSI1 encoder's pixel source).  Split like upstream
 * vc4_crtc: configure() programs every PV register EXCEPT the EN and
 * VIDEN kick bits, which pv_enable()/pv_video_enable() set.  Upstream
 * order (vc4_crtc_atomic_enable): EN, then the DSI host video-mode
 * start, then VIDEN — so:
 *   hvs_bringup -> pv_configure -> pv_enable -> dsi1_video_mode ->
 *   pv_video_enable
 */
int bcm283x_dsi1_pv_configure(const bcm283x_dsi1_adjusted_mode_t* mode);
int bcm283x_dsi1_pv_enable(void);
int bcm283x_dsi1_pv_video_enable(void);
/* Diagnostic: clear WAIT_HSTART so the PV free-runs without the DSI
 * host hstart handshake (see dsi1_pv.c). */
int bcm283x_dsi1_pv_clear_wait_hstart(void);

#endif
