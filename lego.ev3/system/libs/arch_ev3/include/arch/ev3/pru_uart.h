#ifndef __EV3_PRU_UART_H__
#define __EV3_PRU_UART_H__

#include <stdint.h>
#include <ewoksys/ewokdef.h>

/*
 * EV3 input ports 3/4 PRU0 soft-UART (TI "PRU SUART Emulation") host driver.
 *
 * This is the transport layer behind the ev3_uart_* sentinel dispatch in
 * uart.c: when ev3_input_port_uart_base(3/4) hands back a PRU sentinel instead
 * of a 16550 MMIO base, every ev3_uart_*(base) call is routed here and translated
 * into the TI SUART host protocol (McASP serialiser bit-banging driven by PRU0).
 * The contract mirrors the 16550 path exactly so uart_sensor.c is unchanged.
 *
 * It is a straight port of the authoritative TI sources - do NOT invent offsets:
 *   suart_api.h / suart_api.c   (channel block, write/read/read_data, INTC)
 *   suart_utils.c               (suart_mcasp_config + baud divider tables)
 *   pru.c                       (load/run/enable sequence)
 *   lego-linux-drivers omapl_pru_suart.c (probe/startup/irq/set_termios flow)
 * The PRUSS INTC register offsets are cross-validated against EwokOS's own
 * CP-INTC driver (kernel/bsp/irq.c) - same IP - and the PRU_EVTOUT->AINTC line
 * numbers against arch irqs.h (EVTOUT2=5, EVTOUT3=6).
 *
 * Two channels run on PRU0 (PRU1 is left free):
 *   EV3 port 4 -> SUART1 / uartNum 1 / DMA slot 0 / SYS_EVT 34(TX),35(RX)
 *                 -> channel 2 -> host 2 -> PRU_EVTOUT2 -> AINTC IRQ 5
 *   EV3 port 3 -> SUART2 / uartNum 2 / DMA slot 1 / SYS_EVT 36(TX),37(RX)
 *                 -> channel 3 -> host 3 -> PRU_EVTOUT3 -> AINTC IRQ 6
 *
 * !!! ON-BRICK GATE FLAGS !!!
 * The firmware binary is dropped in by the user (see pru_suart_fw.h). A handful
 * of constants below are part of the *firmware ABI* or the *EV3 board wiring*
 * and could not be read from an obtainable authoritative header (TI ships them
 * in suart_pru_regs.h / omapl_suart_board.h, which are not public). They are
 * collected in the two GATE-FLAGGED blocks at the bottom of this file with the
 * derivation + citation for each, and MUST be validated on silicon at the
 * Stage 1/2 gates. Everything above those blocks is verbatim/confirmed.
 */

/* ===================== public transport API =====================
 * `port` is the EV3 input-port index (EV3_IN_PORT_3 == 2, EV3_IN_PORT_4 == 3),
 * i.e. exactly the value uart.c decodes out of the PRU sentinel base. Every
 * function mirrors its ev3_uart_* counterpart's semantics. */

/* Bring up PRUSS + McASP + PRU0 firmware + INTC routing once (idempotent).
 * Returns 0 on success, -1 if the firmware blob is absent (len == 0) or the
 * shared-RAM map failed - in which case ports 3/4 stay inert. */
int ev3_pru_uart_init_subsystem(void);

/* Per-port bring-up/teardown used by port.c's ev3_input_port_uart_enable().
 * port_enable() = init_subsystem() + McASP PINMUX + open the SUART channel
 * (masks, RX host-event, seed the RX buffer). 0 on success, -1 otherwise. */
int  ev3_pru_uart_port_enable(int port);
void ev3_pru_uart_port_disable(int port);

/* ev3_uart_* equivalents (sentinel-dispatched from uart.c). */
uint32_t ev3_pru_uart_get_irq(int port);
void ev3_pru_uart_enable_irq(int port, int dir, int en);
void ev3_pru_uart_init(int port, int baudrate);
void ev3_pru_uart_set_baud(int port, int baudrate);
void ev3_pru_uart_flush_rx(int port);
int  ev3_pru_uart_can_read(int port);
int  ev3_pru_uart_can_write(int port);
int  ev3_pru_uart_tx_empty(int port);
void ev3_pru_uart_putc(int port, char ch);
char ev3_pru_uart_getc(int port);

/* ===================== baud / FIFO policy =====================
 * User decision: ports 3/4 accept a 230400 ceiling (color@460800 stays on the
 * hardware UARTs 1/2). The McASP bit clock is programmed ONCE at the base rate;
 * each channel then divides it down by (BASE / baud), exactly like the lego
 * driver's set_termios -> pru_softuart_setbaud(SUART_DEFAULT_BAUD/baud, ...),
 * only with the base raised 115200 -> 230400 so the full range is reachable. */
#define EV3_PRU_SUART_BASE_BAUD     230400u
#define EV3_PRU_SUART_MIN_BAUD      2400
#define EV3_PRU_SUART_MAX_BAUD      230400
/* TI SUART_FIFO_LEN: the PRU RX FIFO depth posted per read. */
#define EV3_PRU_SUART_FIFO_LEN      15
/* lego: timeout = (BASE_BAUD * SUART_FIFO_TIMEOUT_DFLT) / 1000; DFLT == 10 ms.
 * With the 230400 base this is 2304 (lego's 115200 base gave 1152). */
#define EV3_PRU_SUART_FIFO_TIMEOUT_MS 10
#define EV3_PRU_SUART_FIFO_TIMEOUT  ((EV3_PRU_SUART_BASE_BAUD * EV3_PRU_SUART_FIFO_TIMEOUT_MS) / 1000)
/* Per-channel divisor clamp from pru_softuart_setbaud (rejects 0 and > 385). */
#define EV3_PRU_SUART_DIV_MAX       385

/* ===================== McASP0 register map =====================
 * Offsets relative to McASP0 base (0x01D00000). Verbatim from the TI CSL /
 * davinci-mcasp.h; the absolute XBUF/RBUF/SRCTL bases below are the ones
 * suart_api.h hard-codes (0x01d00200 / 0x01d00280 / 0x01d00180), which pins
 * MCASP_BASE_OFFSET == 0 for this board. */
#define MCASP_PWREMUMGT   0x04
#define MCASP_PFUNC       0x10
#define MCASP_PDIR        0x14
#define MCASP_PDOUT       0x18
#define MCASP_GBLCTL      0x44
#define MCASP_AMUTE       0x48
#define MCASP_DLBCTL      0x4c
#define MCASP_DITCTL      0x50
#define MCASP_RGBLCTL     0x60
#define MCASP_RMASK       0x64
#define MCASP_RFMT        0x68
#define MCASP_AFSRCTL     0x6c
#define MCASP_ACLKRCTL    0x70
#define MCASP_AHCLKRCTL   0x74
#define MCASP_RTDM        0x78
#define MCASP_RINTCTL     0x7c
#define MCASP_RSTAT       0x80
#define MCASP_RCLKCHK     0x88
#define MCASP_XGBLCTL     0xa0
#define MCASP_XMASK       0xa4
#define MCASP_XFMT        0xa8
#define MCASP_AFSXCTL     0xac
#define MCASP_ACLKXCTL    0xb0
#define MCASP_AHCLKXCTL   0xb4
#define MCASP_XTDM        0xb8
#define MCASP_XINTCTL     0xbc
#define MCASP_XSTAT       0xc0
#define MCASP_XCLKCHK     0xc8
#define MCASP_SRCTL(n)    (0x180 + ((n) * 4))
#define MCASP_TXBUF(n)    (0x200 + ((n) * 4))
#define MCASP_RXBUF(n)    (0x280 + ((n) * 4))

/* Absolute McASP buffer/serialiser bases the firmware's per-channel context
 * stores (suart_api.h, MCASP_BASE_OFFSET == 0). */
#define MCASP_XBUF_BASE_ADDR    0x01d00200u
#define MCASP_RBUF_BASE_ADDR    0x01d00280u
#define MCASP_SRCTL_BASE_ADDR   0x01d00180u
#define MCASP_SRCTL_TX_MODE     0x000D
#define MCASP_SRCTL_RX_MODE     0x000E
#define MCASP_SRCTL_INACTIVE    0x000C

/* 230400-base divider words, computed verbatim from suart_utils.c:
 *   tx: ACLKXCTL |= CLKXDIV(0)<<0 ; AHCLKXCTL |= HCLKXDIV(104)<<0
 *   rx(8x): ACLKRCTL |= CLKXDIV(0)<<0 ; AHCLKRCTL |= (HCLKXDIV(13)-1)<<0
 * giving the final register values below (base config OR'd with the divider). */
#define MCASP_ACLKXCTL_230400   0x000000E0u
#define MCASP_AHCLKXCTL_230400  0x00008068u   /* 0x8000 | 104 */
#define MCASP_ACLKRCTL_230400   0x000000A0u
#define MCASP_AHCLKRCTL_230400  0x0000800Cu   /* 0x8000 | (13-1) */

/* ===================== SUART channel block (PRU0 DRAM) =====================
 * pru_suart_regs is a 16-byte block; EV3 uses channels 0..3 at PRU0 DRAM
 * offset 0 (PRU_SUART_PRU0_CH0_OFFSET). The 32-bit bitfield struct in
 * suart_api.h and the byte-offset view the data-path code uses are the same
 * little-endian layout:
 *   0x00 CTRL (2B) | 0x02 CONFIG1 (2B) | 0x04 CONFIG2 (2B) | 0x06 TXRXSTATUS (1B)
 *   0x08 TXRXDATA (4B, physical addr of the host TX/RX buffer)
 *   0x0C BYTESDONECNTR (1B, low byte of Reserved1) */
#define PRU_SUART_PRU0_CH0_OFFSET   0u
#define SUART_NUM_OF_BYTES_PER_CHANNEL  16u
#define SUART_NUM_OF_CHANNELS_PER_SUART 2u

#define PRU_SUART_CH_CTRL_OFFSET            0x00
#define PRU_SUART_CH_CONFIG1_OFFSET         0x02
#define PRU_SUART_CH_CONFIG2_OFFSET         0x04
#define PRU_SUART_CH_TXRXSTATUS_OFFSET      0x06
#define PRU_SUART_CH_TXRXDATA_OFFSET        0x08
#define PRU_SUART_CH_BYTESDONECNTR_OFFSET   0x0C

/* CTRL (2B @0x00) field masks - suart_api.c pru_softuart_write/read. */
#define PRU_SUART_CH_CTRL_MODE_MASK     0x0003
#define PRU_SUART_CH_CTRL_MODE_SHIFT    0
#define PRU_SUART_CH_CTRL_TX_MODE       0x1     /* SUART_CHN_TX */
#define PRU_SUART_CH_CTRL_RX_MODE       0x2     /* SUART_CHN_RX */
#define PRU_SUART_CH_CTRL_SREQ          0x1
#define PRU_SUART_CH_CTRL_SREQ_MASK     0x0004
#define PRU_SUART_CH_CTRL_SREQ_SHIFT    2
#define PRU_SUART_CH_CTRL_SR_MASK       0x0F00  /* serializer_num (bits 8-11) */
#define PRU_SUART_CH_CTRL_SR_SHIFT      8

/* CONFIG1 (2B @0x02): prescaler (bits 0-9) + per-channel IE masks (bits 12-15). */
#define PRU_SUART_CH_CONFIG1_PRESCALER_MASK 0x03FF

/* CONFIG2 (2B @0x04): bits_per_char (bits 0-3) + data_len (bits 8-11). */
#define PRU_SUART_CH_CONFIG2_BITPERCHAR_MASK 0x000F
#define PRU_SUART_CH_CONFIG2_DATALEN_MASK    0x0F00
#define PRU_SUART_CH_CONFIG2_DATALEN_SHIFT   8

/* TXRXSTATUS (1B @0x06) bits - suart_api.h CHN_TXRX_STATUS_*. */
#define CHN_TXRX_STATUS_TIMEOUT   (1u << 6)
#define CHN_TXRX_STATUS_BI        (1u << 5)
#define CHN_TXRX_STATUS_FE        (1u << 4)
#define CHN_TXRX_STATUS_OVRNERR   (1u << 3)   /* UNERR(TX) / OVRNERR(RX) */
#define CHN_TXRX_STATUS_ERR       (1u << 2)
#define CHN_TXRX_STATUS_CMPLT     (1u << 1)
#define CHN_TXRX_STATUS_RDY       (1u << 0)

/* Per-channel interrupt-enable masks (CONFIG1 bits 12-15) + global (IMR bit 9). */
#define CHN_RX_IE_MASK_OVRN         (1u << 15)
#define CHN_TXRX_IE_MASK_TIMEOUT    (1u << 14)
#define CHN_TXRX_IE_MASK_BI         (1u << 13)
#define CHN_TXRX_IE_MASK_FE         (1u << 12)
#define CHN_TXRX_IE_MASK_CMPLT      (1u << 1)
#define SUART_GBL_INTR_ERR_MASK     (1u << 9)

/* Channel direction / state (suart_api.h enums) + 8N1 bits-per-char encoding
 * (ePRU_SUART_DATA_BITS8 == 10 == 8 data + start + stop). */
#define SUART_CHN_TX        1u
#define SUART_CHN_RX        2u
#define SUART_CHN_ENABLED   1u
#define SUART_DATA_BITS8    10u
#define SUART_DEFAULT_OVRSMPL 1u    /* SUART_8X_OVRSMPL */

/* PRU<->ARM interrupt modes (suart_api.h). */
#define PRU_TX_INTR     1u
#define PRU_RX_INTR     2u

/* RX dummy-dump address the PRU writes to before a buffer is posted, so stray
 * RX cannot corrupt DRAM (suart_api.h RX_DEFAULT_DATA_DUMP_ADDR). */
#define RX_DEFAULT_DATA_DUMP_ADDR   0x000001FCu

/* ===================== PRUSS INTC (relative to the INTC block) ============
 * Offsets confirmed against EwokOS's CP-INTC driver (kernel/bsp/irq.c) - the
 * PRUSS INTC is the same IP. Accessed via ev3_pru_intc_read/write(reg). */
#define PRU_INTC_GLBLEN           0x10
#define PRU_INTC_STATIDXSET       0x20
#define PRU_INTC_STATIDXCLR       0x24
#define PRU_INTC_ENIDXSET         0x28
#define PRU_INTC_ENIDXCLR         0x2C
#define PRU_INTC_HSTINTENIDXSET   0x34
#define PRU_INTC_HSTINTENIDXCLR   0x38
#define PRU_INTC_STATCLRINT1      0x204   /* == SYS_RAW_STAT(1): sys events 32-63 */
#define PRU_INTC_CHANMAP7         0x41C
#define PRU_INTC_CHANMAP8         0x420
#define PRU_INTC_CHANMAP9         0x424
#define PRU_INTC_CHANMAP10        0x428
#define PRU_INTC_CHANMAP11        0x42C
#define PRU_INTC_CHANMAP12        0x430
#define PRU_INTC_HOSTMAP0         0x800
#define PRU_INTC_HOSTMAP1         0x804
#define PRU_INTC_HOSTMAP2         0x808
#define PRU_INTC_HOSTINTLVL_MAX   9

/* System-event routing (suart_api.c get_isrstatus / arm_to_pru_intr and the
 * CHANMAP values in arm_to_pru_intr_init): SUART0 TX/RX are SYS_EVT34/35 and
 * each further SUART shifts by 2. The ARM->PRU "service request" doorbell is
 * SYS_EVT32. */
#define PRU_SUART0_TX_EVT       34u
#define PRU_SUART0_TX_EVT_BIT   0x4u    /* bit in STATCLRINT1 for SYS_EVT34 */
#define PRU_SUART0_RX_EVT_BIT   0x8u    /* bit in STATCLRINT1 for SYS_EVT35 */
#define PRU_ARM_TO_PRU_EVT      0x20u   /* SYS_EVT32 */

/* ======================================================================== *
 *  GATE-FLAGGED BLOCK 1 - firmware ABI (PRU0 DRAM control region)          *
 *                                                                          *
 *  These offsets live in TI's suart_pru_regs.h, which is not public. They   *
 *  are DERIVED from the standard PRU_SUART firmware DRAM layout: the 8       *
 *  16-byte channel blocks occupy 0x00..0x7F, so the control region begins   *
 *  at 0x80. Field sizes/order are taken from how suart_api.c reads/writes   *
 *  each (1- or 2-byte accesses at the named symbol). MUST be verified       *
 *  against the dropped-in firmware at the Stage 1 gate; if the blob uses a  *
 *  different control-region base, only these six defines change.            *
 * ======================================================================== */
#define PRU_SUART_PRU0_RX_TX_MODE           0x80  /* 1B: PRU_MODE_RX_TX_BOTH == 3 */
#define PRU_SUART_PRU0_DELAY_OFFSET         0x81  /* 1B: bit-bang delay count     */
#define PRU_SUART_PRU0_ISR_OFFSET           0x82  /* 2B; clr_isrstatus uses +1    */
#define PRU_SUART_PRU0_IDLE_TIMEOUT_OFFSET  0x84  /* 2B: FIFO idle timeout        */
#define PRU_SUART_PRU0_ID_ADDR              0x86  /* 1B: PRU id (0 for PRU0)      */
#define PRU_SUART_PRU0_IMR_OFFSET           0x88  /* 2B: per-channel CMPLT + gbl  */

#define PRU_MODE_RX_TX_BOTH     0x3u
#define SUART_PRU_ID_MASK       0xFFu

/* ======================================================================== *
 *  GATE-FLAGGED BLOCK 2 - EV3 board wiring (serialisers, PINMUX, FIFO)     *
 *                                                                          *
 *  TI ships these in omapl_suart_board.h (not public). Values derived from  *
 *  the EV3 schematic + ev3dev/lego board files in prior research:           *
 *   - port 4 (SUART1): TX on McASP AXR3, RX on AXR1                         *
 *   - port 3 (SUART2): TX on McASP AXR4, RX on AXR2                         *
 *   - McASP master clock/frame pins (ACLKX/AHCLKX/AFSX/ACLKR/AHCLKR/AFSR,   *
 *     bits 26-31) are outputs -> PDIR val 0xFC000000; PFUNC reset val 0.    *
 *   - PINMUX routes AXR1..AXR4 + AHCLKR/AHCLKX to their McASP mode (1).     *
 *   - The ARM<->PRU FIFOs are staged in the on-chip shared SRAM that pru.c   *
 *     already maps (0x80000000). AM1808 errata: local RAM is bus-mastered   *
 *     by ARM + PRU0 only and is coherent (uncached) - ideal for this. If    *
 *     the dropped-in firmware expects its buffers elsewhere, change         *
 *     EV3_PRU_SUART_FIFO_PHYS (and pru.c's shared-RAM map) at the gate.     *
 * ======================================================================== */
#define PRU_SUART1_CONFIG_TX_SER    3u   /* port 4 TX == AXR3 */
#define PRU_SUART1_CONFIG_RX_SER    1u   /* port 4 RX == AXR1 */
#define PRU_SUART2_CONFIG_TX_SER    4u   /* port 3 TX == AXR4 */
#define PRU_SUART2_CONFIG_RX_SER    2u   /* port 3 RX == AXR2 */

#define MCASP_PDIR_VAL              0xFC000000u  /* clock/frame pins = output */
#define CSL_MCASP_PFUNC_RESETVAL    0x00000000u

/* PINMUX (SYSCFG0, privileged - written via ev3_syscfg_write). reg/val/mask
 * are (phys, value, mask) triples for the masked read-modify-write. */
#define EV3_PRU_PINMUX_AHBCLK_REG   0x01C14120u  /* PINMUX0: AHCLKR/AHCLKX */
#define EV3_PRU_PINMUX_AHBCLK_VAL   0x00110000u
#define EV3_PRU_PINMUX_AHBCLK_MASK  0x00FF0000u
#define EV3_PRU_PINMUX_AXR_REG      0x01C14128u  /* PINMUX2: AXR1..AXR4 */
#define EV3_PRU_PINMUX_AXR_VAL      0x01111000u
#define EV3_PRU_PINMUX_AXR_MASK     0x0FFF0000u

/* Physical base of the ARM<->PRU TX/RX FIFO staging area (== pru.c shared RAM).
 * Layout mirrors lego: slot i gets tx = base + 1024*i, rx = tx + 512. */
#define EV3_PRU_SUART_FIFO_PHYS     0x80000000u
#define EV3_PRU_SUART_FIFO_SLOT_SZ  1024u
#define EV3_PRU_SUART_FIFO_HALF     512u

/* PRU core clock (MHz) used only to pick the bit-bang delay byte; the TI
 * default (delay = 3) is used for any frequency that is not 228/186 MHz. */
#define EV3_PRU_CLK_FREQ_MHZ        0u

#endif /* __EV3_PRU_UART_H__ */
