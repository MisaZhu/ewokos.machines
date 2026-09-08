#ifndef __DSI_FBDISPLAYD_PANEL_UC_H__
#define __DSI_FBDISPLAYD_PANEL_UC_H__

#include <stdint.h>
#include <arch/bcm2712/rp1_dsi.h>

/*
 * ClockworkPi MIPI DSI panel family (uConsole / DevTerm), Raspberry Pi 5
 * (BCM2712 / RP1) port of the raspix panel_uc.
 *
 * Unlike the two I2C-controlled families dsi_fbdisplayd.c otherwise drives
 * (PANEL_WS's MCU and PANEL_RPI7's ATTINY + TC358762 bridge), these are
 * raw DSI panels with a DDIC that must be initialised by a vendor DCS
 * command sequence over the link itself:
 *
 *   cwu50      uConsole 5"      720x1280  JD9365DA-H3  newer batch
 *   cwu50_old  uConsole 5"      720x1280  JD9365DA-H3  original batch
 *   cwd686     DevTerm 6.86"    480x1280  ICNL9707
 *
 * cwu50 and cwu50_old are the SAME glass model in two hardware batches.
 * Both carry a byte-identical _mode_cwu50 (720x1280, hfp 43 / hsw 20 /
 * hbp 20, vfp 8 / vsw 2 / vbp 16, 62.5 MHz pixel clock, 4 lanes); they
 * differ ONLY in the DDIC init table, and the two tables are mutually
 * exclusive -- neither batch lights on the other's sequence.
 *
 * All three are 4-lane RGB888 with a continuous HS clock and, per the
 * ClockworkPi DT overlays, identical peripheral wiring:
 *   reset      GPIO 8, active low (DT flag 1 = GPIO_ACTIVE_LOW, so every
 *              logical gpiod level is inverted on the physical pin)
 *   backlight  OCP8178 single-wire on GPIO 9 (bit-banged, NOT I2C/PWM)
 *   panel rails AXP223 PMIC at I2C 0x34 on GPIO0/GPIO1 (ALDO2 =
 *              "display-vcc"); the glass is NOT powered by the CM5
 * So one panel key selects the whole matched set.
 *
 * WHERE THIS DIFFERS FROM THE raspix PORT
 * ---------------------------------------
 * On Pi5 the DSI host lives in the RP1 southbridge, not the BCM283x SoC.
 * The whole clock + D-PHY + host + scan-out layer that the raspix port
 * hand-rolls in panel_uc_dsi.c (PLLD, HVS, PixelValve) is replaced here
 * by the shared arch library:
 *   bcm2712_rp1_dsi_init()         clocks + D-PHY + host -> LP-11 (cmd mode)
 *   bcm2712_rp1_dsi_video_start()  ArgonDPI DMA scans the framebuffer out
 *                                  of host RAM, host switches to video mode
 * The HS bit clock is derived inside bcm2712_rp1_dsi_init() from
 * pixel_clock_hz * 24bpp / (8 * lanes): cwu50 -> 375 MHz, cwd686 ->
 * 326.79 MHz, exactly the constants the raspix tree hardcoded, so this
 * port carries no uc_panel_hs_clock() and no panel_uc_dsi.c at all.
 * The DCS transport (uc_dcs_write) rides bcm2712_rp1_dsi_cmd_short /
 * _cmd_write; GPIO reset + OCP8178 backlight ride bcm2712_gpio; the
 * AXP223 rails ride the RP1 hardware i2c0 on GPIO0/GPIO1.
 */

#define UC_PANEL_CWU50      0
#define UC_PANEL_CWD686     1
#define UC_PANEL_CWU50OLD   2

/* OCP8178 brightness is a 5-bit code looked up in a fixed table, so the
 * user-visible level is an index 0..9 (ocp8178_bl.c MAX_BRIGHTNESS_VALUE).
 * Default 5 matches devterm-panel-uc-overlay. */
#define UC_BACKLIGHT_MAX_LEVEL  9
#define UC_BACKLIGHT_DEFAULT    5

#define UC_PANEL_RESET_GPIO     8
#define UC_BACKLIGHT_GPIO       9

/*
 * Delays.  The RP1 arch library exposes no bcm283x_dsi1_mdelay/udelay
 * equivalent, so panel_uc.c provides these (usleep-backed) and the three
 * vendor tables call them in place of the raspix bcm283x_dsi1_*delay.
 */
void uc_mdelay(uint32_t ms);
void uc_udelay(uint32_t us);

/*
 * Map a display.json "panel" value onto a family id.  Returns -1 when the
 * name is not a ClockworkPi panel, so the caller can fall through to its
 * own ws/rpi7 handling.
 */
int uc_panel_from_name(const char* name);

const char* uc_panel_name(int which);

/*
 * The panel's mode (timings + link parameters), transcribed from the
 * matching panel-*.c driver.  pixel_clock_hz is the nominal pixel clock
 * the vendor declares; bcm2712_rp1_dsi_init() turns it into the HS bit
 * clock (pixel_clock * 24 / (8 * lanes)) with no rounding step of our own.
 *
 * cwu50 and cwu50_old share one mode struct: the two hardware batches
 * were verified to carry byte-identical timings upstream.
 */
const bcm2712_dsi_mode_t* uc_panel_mode(int which);

/*
 * Early panel-side setup, in the order the standalone uConsole daemon
 * proved on hardware:
 *   1. claim the GPIO 8 reset pin at its probe-time level;
 *   2. light the OCP8178 backlight to UC_BACKLIGHT_DEFAULT -- it is on a
 *      separate rail from the glass, and having it live early is what
 *      makes uc_backlight_blink() usable as a failure indicator;
 *   3. bring up the AXP223 display rails.
 *
 * Step 3 MUST complete before any DSI PHY activity: driving LP-11 into
 * an unpowered DDIC parasitically powers it through its ESD diodes, so
 * the real rail arrival never triggers a clean POR and the panel latches
 * deaf to reset, DCS and BTA.  A rail failure is not fatal -- the PMIC
 * defaults may already hold them on, and the DCS table is the real
 * verdict -- so it is logged, not returned.
 */
void uc_panel_prepare(void);

/*
 * HW reset pulse.  MUST run with the DSI host already parked in LP-11
 * (i.e. after bcm2712_rp1_dsi_init()), immediately before the DCS table,
 * so the DDIC comes out of reset seeing a live bus.
 */
void uc_panel_reset_pulse(void);

/*
 * Run the vendor DCS init sequence for the family.  Returns the number of
 * commands that hard-failed on the wire; 0 means the table went out clean.
 *
 * PROVENANCE -- the uConsole tree held THREE tables and they are NOT
 * interchangeable, so do not re-copy from the wrong one:
 *
 *   panel_uc_cwu50.c     <- fbdisplay6d/uc_cwu50.c   newer batch (proven)
 *   panel_uc_cwu50old.c  <- fbdisplayd/uc_cwu50.c    original batch
 *   panel_uc_cwd686.c    <- fbdisplayd/uc_cwd686.c   (only source)
 *
 * Feeding a batch the other's table leaves the DDIC mis-programmed --
 * backlight on, no image -- and no software-side probe can detect it.
 * Neither batch is auto-detectable, so the conf "panel" key is the sole
 * switch.
 */
int uc_panel_init_table(int which);

/* OCP8178 single-wire backlight on GPIO 9. */
void uc_backlight_init(void);
void uc_backlight_set(uint8_t level);   /* 0..UC_BACKLIGHT_MAX_LEVEL */

/*
 * Finite blink code for the fatal bring-up path: n off/on pulses on the
 * backlight, then a gap.  Unlike the standalone uConsole daemon this
 * returns, so a failed init still tears the daemon down normally instead
 * of hanging in an infinite panic loop.
 */
void uc_backlight_blink(uint32_t n);

/*
 * DCS transport shared by the three vendor tables.  Routes by data type:
 *   0x05 (short write, 0 param) / 0x15 (short write, 1 param) go through
 *   bcm2712_rp1_dsi_cmd_short() (params ride in the packet header);
 *   0x39 (long write) goes through bcm2712_rp1_dsi_cmd_write() (payload
 *   rides the FIFO).  Returns 0 on a clean send, negative on hard failure.
 */
int uc_dcs_write(uint8_t data_type, const uint8_t* payload, uint32_t len);

/* Per-family table playback (panel_uc_cwu50.c / panel_uc_cwu50old.c /
 * panel_uc_cwd686.c). */
int uc_cwu50_init(void);
int uc_cwu50old_init(void);
int uc_cwd686_init(void);

#endif
