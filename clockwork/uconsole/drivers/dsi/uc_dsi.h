#ifndef UC_DSI_H
#define UC_DSI_H

#include <stdint.h>

/*
 * BCM2711 DSI1 controller register offsets, from Linux vc4_dsi.c.
 * DSI1 is at MMIO+0x700000 (peripheral offset 0x7e700000 minus 0x7e000000).
 *
 * Phase 2 only reads.  Phase 3 will drive PHY / AFEC / CTRL to bring the
 * link up in LP mode so DCS commands can be sent to the cwu50 panel.
 *
 * NOTE: DSI0 and DSI1 register layouts differ — the definitions here are
 * strictly DSI1 (the one wired to the uConsole panel via the CM4).
 */

#define UC_DSI1_OFFSET          0x700000U

/* DSI1 register offsets (see vc4_dsi.c). */
#define UC_DSI1_CTRL            0x00U
#define UC_DSI1_TXPKT1C         0x04U
#define UC_DSI1_TXPKT1H         0x08U
#define UC_DSI1_TXPKT2C         0x0cU
#define UC_DSI1_TXPKT2H         0x10U
#define UC_DSI1_RXPKT1H         0x14U
#define UC_DSI1_RXPKT2H         0x18U
#define UC_DSI1_TXPKT_CMD_FIFO  0x1cU
#define UC_DSI1_TXPKT_PIX_FIFO  0x20U
#define UC_DSI1_RXPKT_FIFO      0x24U
#define UC_DSI1_DISP0_CTRL      0x28U
#define UC_DSI1_DISP1_CTRL      0x2cU
#define UC_DSI1_INT_STAT        0x30U
#define UC_DSI1_INT_EN          0x34U
#define UC_DSI1_STAT            0x38U
#define UC_DSI1_HSTX_TO_CNT     0x3cU
#define UC_DSI1_LPRX_TO_CNT     0x40U
#define UC_DSI1_TA_TO_CNT       0x44U
#define UC_DSI1_PR_TO_CNT       0x48U
#define UC_DSI1_PHYC            0x4cU
#define UC_DSI1_HS_CLT0         0x50U
#define UC_DSI1_HS_CLT1         0x54U
#define UC_DSI1_HS_CLT2         0x58U
#define UC_DSI1_HS_DLT3         0x5cU
#define UC_DSI1_HS_DLT4         0x60U
#define UC_DSI1_HS_DLT5         0x64U
#define UC_DSI1_HS_DLT6         0x68U
#define UC_DSI1_HS_DLT7         0x6cU
#define UC_DSI1_PHY_AFEC0       0x70U
#define UC_DSI1_PHY_AFEC1       0x74U
#define UC_DSI1_ID              0x8cU

/* CTRL bits. */
#define UC_DSI1_CTRL_EN                 (1U << 0)

/* PHYC bits. */
#define UC_DSI1_PHYC_HS_CLK_CONTINUOUS  (1U << 18)
#define UC_DSI1_PHYC_CLANE_ULPS         (1U << 17)
#define UC_DSI1_PHYC_CLANE_ENABLE       (1U << 16)
#define UC_DSI1_PHYC_DLANE3_ENABLE      (1U << 12)
#define UC_DSI1_PHYC_DLANE2_ENABLE      (1U << 8)
#define UC_DSI1_PHYC_DLANE1_ENABLE      (1U << 4)
#define UC_DSI1_PHYC_DLANE0_ENABLE      (1U << 0)

/* STAT / INT bits used by Phase 3 polling. */
#define UC_DSI1_INT_TXPKT1_DONE         (1U << 1)
#define UC_DSI1_INT_TXPKT1_END          (1U << 0)
/*
 * vc4 keeps the error/timeout interrupts enabled at all times and adds
 * TXPKT1_DONE around each transfer.  INT_STAT reporting may be gated by
 * INT_EN, so we must mirror that even though we poll instead of taking
 * interrupts.  (DSI1_INTERRUPTS_ALWAYS_ENABLED in vc4_dsi.c.)
 */
#define UC_DSI1_INT_ERR_SYNC_ESC        (1U << 6)
#define UC_DSI1_INT_ERR_CONTROL         (1U << 7)
#define UC_DSI1_INT_ERR_CONT_LP0        (1U << 8)
#define UC_DSI1_INT_ERR_CONT_LP1        (1U << 9)
#define UC_DSI1_INT_HSTX_TO             (1U << 10)
#define UC_DSI1_INT_LPRX_TO             (1U << 11)
#define UC_DSI1_INT_TA_TO               (1U << 12)
#define UC_DSI1_INT_PR_TO               (1U << 13)
/* Set when the bus returns to the forward direction after a BTA read. */
#define UC_DSI1_INT_PHY_DIR_RTF         (1U << 21)
/* Set when the bus turns around forward->reverse (BTA actually granted). */
#define UC_DSI1_INT_PHY_DIR_FTR         (1U << 17)
#define UC_DSI1_INT_ALWAYS_ENABLED      (UC_DSI1_INT_ERR_SYNC_ESC | \
                                         UC_DSI1_INT_ERR_CONTROL |  \
                                         UC_DSI1_INT_ERR_CONT_LP0 | \
                                         UC_DSI1_INT_ERR_CONT_LP1 | \
                                         UC_DSI1_INT_HSTX_TO |      \
                                         UC_DSI1_INT_LPRX_TO |      \
                                         UC_DSI1_INT_TA_TO |        \
                                         UC_DSI1_INT_PR_TO)
/*
 * STAT (0x38) carries its own raw TXPKT1 completion bits which are NOT
 * gated by INT_EN ("state reporting bits ... behave like INT_STAT,
 * writing a 1 clears the bit").
 */
#define UC_DSI1_STAT_TXPKT1_DONE        (1U << 1)
#define UC_DSI1_STAT_TXPKT1_BUSY        (1U << 2)
#define UC_DSI1_STAT_PHY_CLOCK_STOP     (1U << 16)
#define UC_DSI1_STAT_PHY_CLOCK_HS       (1U << 17)
#define UC_DSI1_STAT_PHY_CLOCK_ULPS     (1U << 18)
#define UC_DSI1_STAT_PHY_D0_STOP        (1U << 24)
#define UC_DSI1_STAT_PHY_D1_STOP        (1U << 26)
#define UC_DSI1_STAT_PHY_D2_STOP        (1U << 28)
#define UC_DSI1_STAT_PHY_D3_STOP        (1U << 30)

/* PHY_AFEC0 bits (bring-up will only OR/AND against these). */
#define UC_DSI1_PHY_AFEC0_LATCH_ULPS    (1U << 14)

void     uc_dsi_init(void);

uint32_t uc_dsi1_read(uint32_t off);
void     uc_dsi1_write(uint32_t off, uint32_t val);

/* Slog CTRL/STAT/PHYC/AFEC0/AFEC1/INT_STAT/ID. */
void     uc_dsi_dump(void);

/*
 * cwu50 panel:  4 lanes, RGB888 (24bpp), 62.5 MHz pixel clock.
 * vc4_dsi computes phy_clock = pixel_clock * (bpp/lanes) = 375 MHz.
 * We ask the CPRMAN bring-up code for that target and it derives the
 * PLLD_DSI1 divider from the actual VCO.  Both supported panels are
 * 4-lane RGB888; only the HS clock differs (see uc_panel_mode_t).
 */
#define UC_DSI_LANES            4U
#define UC_DSI_FORMAT_RGB888    3U      /* DSI_PFORMAT_RGB888 */
#define UC_DSI_PIXEL_DIVIDER    6U      /* 24bpp / 4 lanes */
#define UC_DSI_HS_CLOCK_HZ      375000000U

/*
 * Override the HS bit clock used for the PHY timing computation in
 * uc_dsi_bringup().  Must be called before uc_dsi_bringup(), with the
 * same value handed to uc_clock_bringup_dsi1().  Defaults to the
 * cwu50 375 MHz above.
 */
void     uc_dsi_set_hs_clock(uint32_t hz);

/*
 * Full DSI1 controller + PHY bring-up matching vc4_dsi_encoder_enable().
 * uc_clock_bringup_dsi1() must have run first.  Leaves DSI1 in command
 * mode with the AFE out of reset and the clock lane in HS if the panel
 * asked for continuous HS clock.
 */
int      uc_dsi_bringup(void);

/*
 * Enter or leave Ultra Low Power State.  Called from bring-up (leave
 * ULPS after the clocks come up) and from Phase 4 whenever we need to
 * quiesce the link between init-table entries.
 */
void     uc_dsi_ulps(int enter);

/*
 * Liveness probes for blink-code diagnosis:
 *  uc_dsi_alive()          0 iff the ID register reads 0x00647369
 *                          ("dsi", same check vc4_dsi probe does).
 *                          Anything else = register bus dead, i.e.
 *                          the firmware power domain never came up.
 *  uc_dsi_lanes_stopped()  0 iff all enabled data lanes report the
 *                          LP-11 STOP state in STAT — the analog PHY
 *                          is actually driving the lines.
 */
int      uc_dsi_alive(void);
int      uc_dsi_lanes_stopped(void);

/*
 * DCS read (DT 0x06 + BTA), mirroring vc4_dsi_host_transfer()'s rx
 * path.  Returns bytes read (short responses give 1-2 bytes straight
 * from RXPKT1H); on timeout returns -1 if the bus turnaround was never
 * granted (PHY_DIR_FTR unseen) or -2 if the bus was handed to the
 * panel but no reply came back (FTR seen, RTF missing).
 * lp != 0 sends the read request in LP escape mode (LPDT) instead of
 * HS.  Reading DCS 0x0A (Get Power Mode) is the definitive panel-side
 * probe: bit4 = sleep-out, bit2 = display-on.
 */
int      uc_dsi_dcs_read(uint8_t cmd, uint8_t* rx, uint32_t rx_len, int lp);

/*
 * Switch subsequent uc_dsi_dcs_write() calls between HS (default, 0)
 * and LP escape mode (1).  Used by fbd to retry the panel init table
 * in LP when the panel ignores the HS command waveform.
 */
void     uc_dsi_set_cmd_lp(int on);

/*
 * Send one DCS/MIPI command over DSI1 in HS mode (cwu50 does not use
 * MIPI_DSI_MODE_LPM, so like upstream all commands go out over the
 * high-speed lanes).  data_type is the
 * MIPI data type byte (e.g. 0x05 for DCS short write, 0x39 for DCS long
 * write).  For short packets payload/len encode param0..param1; for
 * long packets len is the byte count and payload the raw bytes.
 * Returns 0 on success (INT_STAT TXPKT1_DONE observed within timeout).
 */
int      uc_dsi_dcs_write(uint8_t data_type,
		const uint8_t* payload, uint32_t len);

/*
 * Switch DISP0 out of command mode into video mode.  Call after the
 * panel DCS init table has been sent and the panel is ready to see
 * pixels.  This clears DISP0.COMMAND_MODE while keeping DISP0.ENABLE
 * set; the DSI controller will then start consuming pixels from the
 * scan-out source (HVS/PixelValve or the pixel FIFO).
 */
void     uc_dsi_video_mode(void);

#endif
