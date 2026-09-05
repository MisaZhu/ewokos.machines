#ifndef __DSI_FBDISPLAYD_PANEL_UC_H__
#define __DSI_FBDISPLAYD_PANEL_UC_H__

#include <stdint.h>
#include <arch/bcm283x/dsi1.h>

/*
 * ClockworkPi CM4 MIPI DSI panel family (uConsole / DevTerm).
 *
 * Unlike the two I2C-controlled families this daemon otherwise drives
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
 * hbp 20, vfp 8 / vsw 2 / vbp 16, 62.5 MHz pixel clock, 4 lanes,
 * 375 MHz HS clock); they differ ONLY in the DDIC init table, and the
 * two tables are mutually exclusive — neither batch lights on the
 * other's sequence.  Upstream that split was two separate daemons:
 * fbdisplay6d drove the newer batch, fbdisplayd the original one.
 *
 * All three are 4-lane RGB888 on DSI1 with a continuous HS clock and,
 * per the ClockworkPi DT overlays, identical peripheral wiring:
 *   reset      GPIO 8, active low (DT flag 1 = GPIO_ACTIVE_LOW, so every
 *              logical gpiod level is inverted on the physical pin)
 *   backlight  OCP8178 single-wire on GPIO 9 (bit-banged, NOT I2C/PWM)
 *   panel rails AXP223 PMIC at I2C 0x34 on the software bus GPIO0/GPIO1
 *              (ALDO2 = "display-vcc"); the glass is NOT powered by the CM4
 * So one panel key selects the whole matched set.
 *
 * The clock + DSI host layer is NOT the shared arch library — see
 * panel_uc_dsi.c and the section note near the bottom of this header.
 * HVS and PV do stay on the shared library.
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
 * Scan-out window, pinned to the value fbdisplayd/fbdisplay6d hardcode
 * (UCONSOLE_FB_PHYS_BASE).  The ClockworkPi config.txt sets gpu_mem=16
 * so the VC firmware carve starts above this window — the Pi4 default
 * 76MB carve begins at 0x3b400000 and would overlap it.
 *
 * dsi_fbdisplayd.c's generic pick_fb_phys_base() returns the SAME
 * address on a >1GB board, but on a <=1GB CM4 it returns
 * top-of-RAM minus 32MB (0x3e000000) instead — a different window from
 * the one the vendor overlay reserves, so the HVS would scan out memory
 * nobody wrote while every liveness probe still passed.  The uc
 * families therefore pin the proven window rather than inherit one.
 */
#define UC_PANEL_FB_PHYS_BASE   0x3c100000U

/*
 * Map a display.json "panel" value onto a family id.  Returns -1 when the
 * name is not a ClockworkPi panel, so the caller can fall through to its
 * own ws/rpi7 handling.
 */
int uc_panel_from_name(const char* name);

const char* uc_panel_name(int which);

/*
 * The panel's DRM mode (timings + link parameters), transcribed from the
 * matching panel-*.c driver.  pixel_clock_hz is the nominal pixel clock
 * the vendor declares.  The HS bit clock is NOT derived from it here —
 * uc_panel_hs_clock() below returns the vendor constant directly, and the
 * PHY timing bank must be built from exactly the value handed to the
 * clock layer with no rounding step in between.
 *
 * cwu50 and cwu50_old share one mode struct: the two hardware batches
 * were verified to carry byte-identical timings upstream.
 */
const bcm283x_dsi1_mode_t* uc_panel_mode(int which);

/*
 * HS bit clock the vendor driver programs into PLLD_DSI1, i.e.
 * uc_panel_mode_t.hs_clock_hz in the uConsole tree — a hardcoded
 * constant there, NOT derived from the pixel clock at run time:
 *
 *   cwu50    375000000   (fbdisplay6d/uc_panel.c)
 *   cwd686   326790000   (fbdisplayd/uc_cwd686.h UC_CWD686_HS_CLOCK_HZ)
 *
 * Both equal pixel_clock * 24bpp / 4 lanes, so this is the same number
 * the shared library would compute; it is spelled out because the PHY
 * timing bank below must be built from exactly the value handed to the
 * clock layer, with no rounding step in between.
 */
uint32_t uc_panel_hs_clock(int which);

/*
 * Early panel-side setup, in the order the standalone uConsole daemon
 * proved on hardware:
 *   1. claim the GPIO 8 reset pin at its probe-time level (it defaults
 *      to SPI0_CE0 alt-func on CM4, so take it back as a plain output);
 *   2. light the OCP8178 backlight to UC_BACKLIGHT_DEFAULT — it is on a
 *      separate rail from the glass, and having it live early is what
 *      makes uc_backlight_blink() usable as a failure indicator;
 *   3. bring up the AXP223 display rails.
 *
 * Step 3 MUST complete before any DSI PHY activity: driving LP-11 into
 * an unpowered DDIC parasitically powers it through its ESD diodes, so
 * the real rail arrival never triggers a clean POR and the panel latches
 * deaf to reset, DCS and BTA.  Linux never hits this because the AXP
 * regulators are regulator-always-on from early boot.  A rail failure is
 * not fatal — the PMIC defaults may already hold them on, and the DCS
 * table is the real verdict — so it is logged, not returned.
 */
void uc_panel_prepare(void);

/*
 * HW reset pulse.  MUST run with the DSI host already parked in LP-11
 * (i.e. after bcm283x_dsi1_host_bringup()), immediately before the DCS
 * table, so the DDIC comes out of reset seeing a live bus — DRM's
 * host pre_enable -> panel prepare ordering.
 */
void uc_panel_reset_pulse(void);

/*
 * Run the vendor DCS init sequence for the family.  Returns the number of
 * commands whose TXPKT1_DONE never fired; 0 means the table went out
 * clean.
 *
 * PROVENANCE — the uConsole tree held THREE tables and they are NOT
 * interchangeable, so do not re-copy from the wrong one:
 *
 *   panel_uc_cwu50.c     <- fbdisplay6d/uc_cwu50.c   newer batch (proven)
 *   panel_uc_cwu50old.c  <- fbdisplayd/uc_cwu50.c    original batch
 *   panel_uc_cwd686.c    <- fbdisplayd/uc_cwd686.c   (only source)
 *
 * fbdisplayd played the older `_cwu50_seq` (212 entries) and then
 * repeated a second SLPOUT/DSPON pair; fbdisplay6d played the "future
 * LCD" `_cwu50_seq2` (190 entries, self-contained tail) after a SLPOUT +
 * page-0 predetect.  seq2 carries materially different register values
 * (e.g. 0x68/0x69/0x6A = 0x0D/0x06/0x6A instead of 0x06/0x65/0x66, no
 * 0x70..0x7E block, plus page-4 0x02/0x09/0x0E).
 *
 * Feeding a batch the other's table leaves the DDIC mis-programmed —
 * backlight on, no image — and no software-side probe can detect it,
 * because TXPKT1_DONE only proves the controller serialised the packet,
 * never that the panel accepted the values.  Neither batch is
 * auto-detectable: the only readback upstream ever issued (DCS 0x04) had
 * its result discarded and never selected a sequence, so the conf "panel"
 * key is the sole switch.  fbdisplay6d had no cwd686 at all (its DT
 * overlay pins the panel to "cw,cwu50"), so the DevTerm table can only
 * come from fbdisplayd.
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
 * DCS transport shared by the two vendor tables.  Sends in HS command
 * mode (neither panel sets MIPI_DSI_MODE_LPM, so upstream transmits the
 * init table over the high-speed lanes; forcing LP puts a different
 * waveform on the wire and the panel latches nothing).
 */
int uc_dcs_write(uint8_t data_type, const uint8_t* payload, uint32_t len);

/* Per-family table playback (panel_uc_cwu50.c / panel_uc_cwu50old.c /
 * panel_uc_cwd686.c). */
int uc_cwu50_init(void);
int uc_cwu50old_init(void);
int uc_cwd686_init(void);

/*
 * ---------------- clock + DSI host (panel_uc_dsi.c) ----------------
 *
 * The uc families do NOT use the shared library's
 * bcm283x_dsi1_clock_bringup() / _host_bringup() / _cmd_write() /
 * _video_mode().  Those four deviate from the sequence fbdisplay6d
 * proved on this glass (PLLD VCO feedback pre-divider, DSI1E escape
 * clock parent, CTRL_EN ordering, and a per-write gen-detection
 * syscall that routes DSI1 stores through the gen4 broken-AXI DMA
 * workaround); the full list with register-level evidence is in the
 * header comment of panel_uc_dsi.c.  Everything here is a line-for-line
 * transcription of drivers/dsi/uc_clock.c and drivers/fbdisplay6d/
 * uc_dsi.c instead, and touches _mmio_base directly.
 *
 * HVS and PV stay on the shared library: uc_hvs.c / uc_pv.c were
 * compared against bcm283x_dsi1_hvs_bringup() / _pv_configure() and are
 * equivalent for this mode (PV1 fifo_full_level 46, PV_MUX_CFG no-swap,
 * raw panel blanking — cwu50's 3000/375 divides exactly so the shared
 * hfp compensation is a no-op).
 */

/*
 * CPRMAN: PLLD_DSI1 integer divider + DSI1E escape + DSI1P byte clock.
 * Returns the HS bit clock PLLD actually generates, or 0 on failure.
 */
uint32_t uc_clock_bringup_dsi1(uint32_t target_hs_hz);

/* DSI1 ID register must read 0x00647369.  0 = alive. */
int uc_dsi_alive(void);

/*
 * PHY + host bring-up.  hs_clock_hz feeds the HS_CLT and HS_DLT timing
 * bank, so it must be the value uc_clock_bringup_dsi1() returned.
 * Leaves DISP0_CTRL at 0; uc_dsi_video_mode() fills it in after the
 * panel DCS init.  0 = success.
 */
int uc_dsi_bringup(uint32_t hs_clock_hz);

/* All four data lanes must sit in LP-11 STOP.  0 = stopped. */
int uc_dsi_lanes_stopped(void);

/* One-shot DISP0_CTRL video-mode write (PIX_CLK_DIV=6, RGB888). */
void uc_dsi_video_mode(void);

/* Raw DCS write used by uc_dcs_write(); 0 = TXPKT1_DONE fired. */
int uc_dsi_dcs_write(uint8_t data_type, const uint8_t* payload, uint32_t len);

#endif
