#include <stdint.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>

#include "../include/arch/ev3/pru.h"
#include "../include/arch/ev3/pru_uart.h"
#include "../include/arch/ev3/pru_suart_fw.h"
#include "../include/arch/ev3/gpio.h"   /* ev3_syscfg_write (privileged PINMUX) */
#include "../include/arch/ev3/uart.h"   /* EV3_IRQ_RX / EV3_IRQ_TX              */

/*
 * EV3 input ports 3/4 PRU0 soft-UART host driver - see pru_uart.h for the map.
 *
 * Faithful port of the TI PRU SUART host side (suart_api.c / suart_utils.c /
 * pru.c) and the lego-linux-drivers omapl_pru_suart.c bring-up flow, retargeted
 * onto EwokOS's flat-MMIO PRU HAL (pru.c) instead of TI's arm_pru_iomap. Two
 * channels run on PRU0; PRU1 is untouched. The 16550-facing contract (can_read/
 * getc/can_write/putc/tx_empty/flush_rx/set_baud/enable_irq) is reproduced so
 * uart_sensor.c needs no changes - uart.c merely routes the sentinel base here.
 *
 * Deviations from the reference, all deliberate and documented at the site:
 *  - Baud base raised 115200 -> 230400 (accepted ports-3/4 ceiling); the McASP
 *    bit clock is programmed once at 230400 and each channel divides down by
 *    (230400 / baud), mirroring lego's setbaud(DEFAULT/baud, DEFAULT/baud).
 *  - TX is synchronous (bounded-poll + single-byte post) rather than IRQ-driven:
 *    sensor TX is a handful of command bytes, so we skip the TX host-event and
 *    leave the PRU_EVTOUT line purely RX-driven. RX keeps the reference's
 *    posted-double-buffer + timeout-repost behaviour exactly.
 *  - RX bytes are staged in a small per-port software FIFO so can_read()/getc()
 *    match the 16550 "byte at a time" contract uart_sensor.c expects; the FIFO
 *    is filled lazily from the PRU on can_read (which also acks the PRUSS INTC,
 *    so the existing rx_irq drain loop works unchanged).
 *  - TI zeroes PRU DRAM with an unaligned `*(u32*)(base|i)` loop; pru.c notes
 *    that faults on ARM926 Device memory, so we clear aligned words here.
 */

/* PRU0 DRAM control-region writes go through pru.c's byte/word RAM accessors;
 * these thin wrappers keep the ported bodies readable. off is absolute from the
 * PRUSS base (PRU0 DRAM == offset 0). */
static uint8_t  ram_rd8 (uint32_t off){ uint8_t  v=0; ev3_pru_ram_read(off,&v,1); return v; }
static void     ram_wr8 (uint32_t off, uint8_t  v){ ev3_pru_ram_write(off,&v,1); }
static uint16_t ram_rd16(uint32_t off){ uint16_t v=0; ev3_pru_ram_read(off,&v,2); return v; }
static void     ram_wr16(uint32_t off, uint16_t v){ ev3_pru_ram_write(off,&v,2); }
static uint32_t ram_rd32(uint32_t off){ uint32_t v=0; ev3_pru_ram_read(off,&v,4); return v; }
static void     ram_wr32(uint32_t off, uint32_t v){ ev3_pru_ram_write(off,&v,4); }

/* CH_Ctrl_Config1 / CH_Config2_TXRXStatus are u32 bitfields; build the word
 * from the field positions in suart_api.h (mode 0-1, serializer_num 8-11,
 * over_sampling 26-27 ; bits_per_char 0-3, chn_state 31). */
#define CFG1_WORD(mode, ser, ovs) \
    ((uint32_t)(mode) | ((uint32_t)(ser) << 8) | ((uint32_t)(ovs) << 26))
#define CFG2_WORD(bpc, state) \
    ((uint32_t)(bpc) | ((uint32_t)(state) << 31))

/* Per-channel context-block bases inside PRU0 DRAM (pru_set_ram_data): the
 * SUARTn TX context is at 0xB0 + (n-1)*0x50, RX at 0xC0 + (n-1)*0x50, and the
 * TX formatted-data staging buffer at 0x90 + (n-1)*0x50. EV3 uses n = 1,2. */
#define TX_PRIV_OFF(s)   (0xB0u + (uint32_t)(s) * 0x50u)
#define RX_PRIV_OFF(s)   (0xC0u + (uint32_t)(s) * 0x50u)
#define TX_BUFF_ADDR(s)  ((uint16_t)(0x90u + (uint32_t)(s) * 0x50u))

/* Software RX staging FIFO (power-of-two). */
#define PRU_RX_FIFO_SZ   256u
#define PRU_RX_FIFO_MASK (PRU_RX_FIFO_SZ - 1u)

struct pru_suart_slot {
    int      open;
    uint32_t tx_phys, rx_phys;   /* physical addr posted into CH_TXRXData   */
    uint8_t* tx_va;   uint8_t* rx_va;   /* ARM view of the same shared RAM  */
    uint8_t  rx_fifo[PRU_RX_FIFO_SZ];
    volatile uint32_t rx_head, rx_tail;
};

/* slot 0 == EV3 port 4 == SUART1/uartNum1 ; slot 1 == EV3 port 3 == SUART2/uartNum2 */
static struct pru_suart_slot _slot[2];
static int _subsys_up = 0;

/* Map an EV3 input-port index to the internal slot, or -1. */
static int pru_slot(int ev3_port){
    if(ev3_port == 3) return 0;   /* EV3_IN_PORT_4 */
    if(ev3_port == 2) return 1;   /* EV3_IN_PORT_3 */
    return -1;
}
#define UARTNUM(s) ((s) + 1)
/* TX channel == 2*slot, RX channel == 2*slot+1 (== (uartNum*2)-2 and +1). */
#define CH_OFF(ch) (PRU_SUART_PRU0_CH0_OFFSET + (uint32_t)(ch) * SUART_NUM_OF_BYTES_PER_CHANNEL)

/* ===================== McASP0 configuration =====================
 * suart_mcasp_config() verbatim (8x oversample) with the divider words for the
 * 230400 base (suart_utils.c lt_tx_baud_rate / lt_rx_8x_baud_rate). EV3 wires
 * only SUART1/2, so PFUNC/PDIR mark just those two TX serialisers (3 and 4);
 * TI's board lists all eight. */
static void mcasp_config_230400(void){
    uint32_t n;
    uint32_t txsers = (1u << PRU_SUART1_CONFIG_TX_SER) | (1u << PRU_SUART2_CONFIG_TX_SER);

    ev3_pru_mcasp_write(MCASP_GBLCTL, 0);
    ev3_pru_mcasp_write(MCASP_RGBLCTL, 0);
    ev3_pru_mcasp_write(MCASP_XGBLCTL, 0);

    /* receive */
    ev3_pru_mcasp_write(MCASP_RMASK, 0x000000FF);
    ev3_pru_mcasp_write(MCASP_RFMT,  0x0000A038);   /* 8-bit slot, RPAD=1 */
    ev3_pru_mcasp_write(MCASP_AFSRCTL,   0x00000002);   /* burst mode */
    ev3_pru_mcasp_write(MCASP_ACLKRCTL,  0x000000A0);
    ev3_pru_mcasp_write(MCASP_AHCLKRCTL, 0x00008000);
    /* rx_baud_set (8x, 230400): ACLKRCTL |= 0<<0 ; AHCLKRCTL |= (13-1)<<0 */
    ev3_pru_mcasp_write(MCASP_ACLKRCTL,  ev3_pru_mcasp_read(MCASP_ACLKRCTL)  | (0u << 0));
    ev3_pru_mcasp_write(MCASP_AHCLKRCTL, ev3_pru_mcasp_read(MCASP_AHCLKRCTL) | ((13u - 1u) << 0));
    ev3_pru_mcasp_write(MCASP_RTDM,    0x00000001);
    ev3_pru_mcasp_write(MCASP_RINTCTL, 0x00000002);
    ev3_pru_mcasp_write(MCASP_RCLKCHK, 0x00FF0008);

    /* transmit */
    ev3_pru_mcasp_write(MCASP_XMASK,   0x0000FFFF);
    ev3_pru_mcasp_write(MCASP_XFMT,    0x00002078);
    ev3_pru_mcasp_write(MCASP_AFSXCTL,   0x00000002);   /* burst mode */
    ev3_pru_mcasp_write(MCASP_ACLKXCTL,  0x000000E0);
    ev3_pru_mcasp_write(MCASP_AHCLKXCTL, 0x00008000);
    /* tx_baud_set (230400): ACLKXCTL |= 0<<0 ; AHCLKXCTL |= 104<<0 */
    ev3_pru_mcasp_write(MCASP_ACLKXCTL,  ev3_pru_mcasp_read(MCASP_ACLKXCTL)  | (0u << 0));
    ev3_pru_mcasp_write(MCASP_AHCLKXCTL, ev3_pru_mcasp_read(MCASP_AHCLKXCTL) | (104u << 0));
    ev3_pru_mcasp_write(MCASP_XTDM,    0x00000001);
    ev3_pru_mcasp_write(MCASP_XINTCTL, 0x00000002);
    ev3_pru_mcasp_write(MCASP_XCLKCHK, 0x00FF0008);

    /* all serialisers start inactive; ram_data_init() activates the four used */
    for(n = 0; n < 16; n++)
        ev3_pru_mcasp_write(MCASP_SRCTL(n), MCASP_SRCTL_INACTIVE);

    /* TX serialiser pins to GPIO by default (the PRU flips them to McASP mode
     * just before a transfer); clock/frame pins are outputs. */
    ev3_pru_mcasp_write(MCASP_PFUNC, CSL_MCASP_PFUNC_RESETVAL | txsers);
    ev3_pru_mcasp_write(MCASP_PDOUT, 0xFFFF);
    ev3_pru_mcasp_write(MCASP_PDIR,  0x00000000);
    ev3_pru_mcasp_write(MCASP_PDIR,  txsers | MCASP_PDIR_VAL);
    ev3_pru_mcasp_write(MCASP_PDOUT, 0xFFFF);

    ev3_pru_mcasp_write(MCASP_DITCTL, 0);
    ev3_pru_mcasp_write(MCASP_DLBCTL, 0);
    ev3_pru_mcasp_write(MCASP_AMUTE,  0);
    ev3_pru_mcasp_write(MCASP_XSTAT,  0x0000FFFF);
    ev3_pru_mcasp_write(MCASP_RSTAT,  0x0000FFFF);
}

static void serializer_deactivate(uint32_t sr){
    if(sr <= 15)
        ev3_pru_mcasp_write(MCASP_SRCTL(sr), MCASP_SRCTL_INACTIVE);
}

/* ===================== PRU0 DRAM channel/context layout =====================
 * pru_set_ram_data() for the two EV3 channels (SUART1 -> slot0, SUART2 -> slot1).
 * Channel blocks are 16 B at CH_OFF(); TX context at 0xB0+0x50*s, RX at 0xC0+0x50*s. */
static void ram_data_init(void){
    int s;
    for(s = 0; s < 2; s++){
        uint32_t txser = s ? PRU_SUART2_CONFIG_TX_SER : PRU_SUART1_CONFIG_TX_SER;
        uint32_t rxser = s ? PRU_SUART2_CONFIG_RX_SER : PRU_SUART1_CONFIG_RX_SER;
        uint32_t off;

        /* ---- TX channel (2s) ---- */
        off = CH_OFF(2 * s);
        ram_wr32(off + PRU_SUART_CH_CTRL_OFFSET,
                 CFG1_WORD(SUART_CHN_TX, txser, SUART_DEFAULT_OVRSMPL));
        ram_wr32(off + PRU_SUART_CH_CONFIG2_OFFSET, CFG2_WORD(8, SUART_CHN_ENABLED));
        ram_wr32(off + PRU_SUART_CH_TXRXDATA_OFFSET, 0);
        ram_wr32(off + PRU_SUART_CH_BYTESDONECNTR_OFFSET, 1);   /* Reserved1 = 1 */
        ev3_pru_mcasp_write(MCASP_SRCTL(txser), MCASP_SRCTL_TX_MODE);

        off = TX_PRIV_OFF(s);
        ram_wr32(off + 0, MCASP_SRCTL_BASE_ADDR + (txser << 2));  /* asp_xsrctl_base */
        ram_wr32(off + 4, MCASP_XBUF_BASE_ADDR  + (txser << 2));  /* asp_xbuf_base   */
        ram_wr16(off + 8, TX_BUFF_ADDR(s));                       /* buff_addr       */

        /* ---- RX channel (2s+1) ---- */
        off = CH_OFF(2 * s + 1);
        ram_wr32(off + PRU_SUART_CH_CTRL_OFFSET,
                 CFG1_WORD(SUART_CHN_RX, rxser, SUART_DEFAULT_OVRSMPL));
        ram_wr32(off + PRU_SUART_CH_CONFIG2_OFFSET, CFG2_WORD(8, SUART_CHN_ENABLED));
        ram_wr32(off + PRU_SUART_CH_TXRXDATA_OFFSET, RX_DEFAULT_DATA_DUMP_ADDR);
        ram_wr32(off + PRU_SUART_CH_BYTESDONECNTR_OFFSET, 0);   /* Reserved1 = 0 */
        ev3_pru_mcasp_write(MCASP_SRCTL(rxser), MCASP_SRCTL_RX_MODE);

        off = RX_PRIV_OFF(s);
        ram_wr32(off + 0, MCASP_RBUF_BASE_ADDR  + (rxser << 2));  /* asp_rbuf_base   */
        ram_wr32(off + 4, MCASP_SRCTL_BASE_ADDR + (rxser << 2));  /* asp_rsrctl_base */
    }
}

/* ===================== PRUSS INTC =====================
 * arm_to_pru_intr_init() verbatim for the RX_TX_BOTH (SINGLE_PRU) path. Routes
 * SYS_EVT34/35 -> channel 2 -> host 2 -> PRU_EVTOUT2 and SYS_EVT36/37 ->
 * channel 3 -> host 3 -> PRU_EVTOUT3; SYS_EVT32 is the ARM->PRU doorbell.
 * The PRU_EVTOUT lines then reach the ARM CP-INTC (kernel/bsp/irq.c) as sysints
 * 5/6, which the kernel already fans onto nIRQ - no kernel change needed. */
static void intc_init(void){
    int i;
    for(i = 0; i <= PRU_INTC_HOSTINTLVL_MAX; i++)
        ev3_pru_intc_write(PRU_INTC_HSTINTENIDXCLR, (uint32_t)i);
    ev3_pru_intc_write(PRU_INTC_GLBLEN, 0x1);
    for(i = 0; i <= PRU_INTC_HOSTINTLVL_MAX; i++)
        ev3_pru_intc_write(PRU_INTC_HSTINTENIDXSET, (uint32_t)i);

    ev3_pru_intc_write(PRU_INTC_HOSTMAP0, 0x03020100);
    ev3_pru_intc_write(PRU_INTC_HOSTMAP1, 0x07060504);
    ev3_pru_intc_write(PRU_INTC_HOSTMAP2, 0x00000908);

    ev3_pru_intc_write(PRU_INTC_CHANMAP7,  0x00000000);
    ev3_pru_intc_write(PRU_INTC_CHANMAP8,  0x02020100);   /* EVT32->c0 33->c1 34->c2 35->c2 */
    ev3_pru_intc_write(PRU_INTC_CHANMAP9,  0x04040303);   /* EVT36->c3 37->c3 38->c4 39->c4 */
    ev3_pru_intc_write(PRU_INTC_CHANMAP10, 0x06060505);
    ev3_pru_intc_write(PRU_INTC_CHANMAP11, 0x08080707);
    ev3_pru_intc_write(PRU_INTC_CHANMAP12, 0x00010909);

    for(i = 0; i < 18; i++)
        ev3_pru_intc_write(PRU_INTC_STATIDXCLR, (uint32_t)(32 + i));
    /* enable only the ARM->PRU doorbell events; PRU->host events are enabled
     * per-channel on demand (suart_pru_to_host_intr_enable). */
    ev3_pru_intc_write(PRU_INTC_ENIDXSET, 31);
    ev3_pru_intc_write(PRU_INTC_ENIDXSET, 32);
    ev3_pru_intc_write(PRU_INTC_ENIDXSET, 33);
    ev3_pru_intc_write(PRU_INTC_ENIDXSET, 50);

    ev3_pru_intc_write(PRU_INTC_GLBLEN, 0x1);
    for(i = 0; i <= PRU_INTC_HOSTINTLVL_MAX; i++)
        ev3_pru_intc_write(PRU_INTC_HSTINTENIDXSET, (uint32_t)i);
}

/* suart_arm_to_pru_intr(): ring the PRU doorbell (SYS_EVT32) so it picks up a
 * freshly-posted service request. */
static void arm_to_pru_intr(int uartNum){
    (void)uartNum;   /* RX_TX_BOTH, uartNum 1..4 all use SYS_EVT32 */
    ev3_pru_intc_write(PRU_INTC_STATIDXSET, PRU_ARM_TO_PRU_EVT);
}

/* suart_pru_to_host_intr_enable(): enable/disable a channel's PRU->host system
 * event (34+chnNum) so it can reach PRU_EVTOUTn -> the ARM AINTC line. */
static void pru_to_host_intr_enable(int uartNum, int rx, int flag){
    uint32_t chnNum = (uint32_t)(uartNum * 2 - 2);
    uint32_t value;
    if(rx) chnNum++;
    value = PRU_SUART0_TX_EVT + chnNum;   /* 34 + chnNum */
    ev3_pru_intc_write(flag ? PRU_INTC_ENIDXSET : PRU_INTC_ENIDXCLR, value);
}

/* ===================== channel control (suart_api.c ports) ===================== */

/* pru_softuart_setbaud(): write the prescaler into both the TX and RX channel
 * CONFIG1 words. NOTE the TI quirk (suart_api.c L994): the RX channel is
 * programmed with txClkDivisor too, so we pass one divisor for both. */
static void suart_setbaud(int s, uint16_t div){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2);
    uint32_t off;
    uint16_t v;

    if(div == 0 || div > EV3_PRU_SUART_DIV_MAX)
        return;
    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG1_OFFSET;
    v = ram_rd16(off); v &= ~PRU_SUART_CH_CONFIG1_PRESCALER_MASK; v |= div; ram_wr16(off, v);
    chNum++;   /* RX channel reuses the same divisor */
    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG1_OFFSET;
    v = ram_rd16(off); v &= ~PRU_SUART_CH_CONFIG1_PRESCALER_MASK; v |= div; ram_wr16(off, v);
}

/* pru_softuart_setdatabits(): bits_per_char is the low nibble of CONFIG2 byte 0.
 * 8N1 == ePRU_SUART_DATA_BITS8 == 10 (8 data + start + stop). */
static void suart_setdatabits(int s, uint8_t bits){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2);
    uint32_t off;
    uint8_t v;

    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG2_OFFSET;
    v = ram_rd8(off); v &= ~0xF; v |= bits; ram_wr8(off, v);
    chNum++;
    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG2_OFFSET;
    v = ram_rd8(off); v &= ~0xF; v |= bits; ram_wr8(off, v);
}

/* pru_softuart_write(): stage dataLen at buf_phys, set CTRL = TX|SREQ, doorbell. */
static void suart_write(int s, uint32_t buf_phys, uint16_t dataLen){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2);
    uint32_t off;
    uint16_t v;

    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG2_OFFSET;
    v = ram_rd16(off);
    v &= ~PRU_SUART_CH_CONFIG2_DATALEN_MASK;
    v |= (uint16_t)(dataLen << PRU_SUART_CH_CONFIG2_DATALEN_SHIFT);
    ram_wr16(off, v);

    off = CH_OFF(chNum) + PRU_SUART_CH_TXRXDATA_OFFSET;
    ram_wr32(off, buf_phys);

    off = CH_OFF(chNum) + PRU_SUART_CH_CTRL_OFFSET;
    v = ram_rd16(off);
    v &= ~(PRU_SUART_CH_CTRL_MODE_MASK | PRU_SUART_CH_CTRL_SREQ_MASK);
    v |= (PRU_SUART_CH_CTRL_TX_MODE << PRU_SUART_CH_CTRL_MODE_SHIFT) |
         (PRU_SUART_CH_CTRL_SREQ    << PRU_SUART_CH_CTRL_SREQ_SHIFT);
    ram_wr16(off, v);

    arm_to_pru_intr(uartNum);
}

/* suart_intr_setmask() / clrmask(): CMPLT -> per-channel bit in the shared IMR;
 * GBL_ERR -> IMR bit 9; FE/BI/TIMEOUT/OVRN -> per-channel CONFIG1 bits 12-15. */
static void suart_intr_mask(int uartNum, int rx, uint32_t mask, int set){
    uint32_t chnNum = (uint32_t)(uartNum * 2 - 2);
    uint32_t regval, off, v;

    if(rx) chnNum++;
    regval = 1u << chnNum;

    if(mask & CHN_TXRX_IE_MASK_CMPLT){
        v = ram_rd16(PRU_SUART_PRU0_IMR_OFFSET);
        if(set) v |= regval; else v &= ~regval;
        ram_wr16(PRU_SUART_PRU0_IMR_OFFSET, (uint16_t)v);
    }
    if(mask & SUART_GBL_INTR_ERR_MASK){
        v = ram_rd16(PRU_SUART_PRU0_IMR_OFFSET);
        if(set) v |= SUART_GBL_INTR_ERR_MASK; else v &= ~SUART_GBL_INTR_ERR_MASK;
        ram_wr16(PRU_SUART_PRU0_IMR_OFFSET, (uint16_t)v);
    }
    off = CH_OFF(chnNum) + PRU_SUART_CH_CONFIG1_OFFSET;
    if(mask & CHN_TXRX_IE_MASK_FE){
        v = ram_rd16(off); if(set) v |= CHN_TXRX_IE_MASK_FE; else v &= ~CHN_TXRX_IE_MASK_FE; ram_wr16(off,(uint16_t)v);
    }
    if(mask & CHN_TXRX_IE_MASK_BI){
        v = ram_rd16(off); if(set) v |= CHN_TXRX_IE_MASK_BI; else v &= ~CHN_TXRX_IE_MASK_BI; ram_wr16(off,(uint16_t)v);
    }
    if(mask & CHN_TXRX_IE_MASK_TIMEOUT){
        v = ram_rd16(off); if(set) v |= CHN_TXRX_IE_MASK_TIMEOUT; else v &= ~CHN_TXRX_IE_MASK_TIMEOUT; ram_wr16(off,(uint16_t)v);
    }
    if(mask & CHN_RX_IE_MASK_OVRN){
        v = ram_rd16(off); if(set) v |= CHN_RX_IE_MASK_OVRN; else v &= ~CHN_RX_IE_MASK_OVRN; ram_wr16(off,(uint16_t)v);
    }
}
static void suart_intr_setmask(int uartNum, int rx, uint32_t mask){ suart_intr_mask(uartNum, rx, mask, 1); }
static void suart_intr_clrmask(int uartNum, int rx, uint32_t mask){ suart_intr_mask(uartNum, rx, mask, 0); }

/* pru_softuart_read(): post the RX double-buffer and arm the timeout interrupt. */
static void suart_post_read(int s, uint32_t buf_phys, uint16_t dataLen){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2 + 1);   /* RX channel */
    uint32_t off;
    uint16_t v;

    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG2_OFFSET;
    v = ram_rd16(off);
    v &= ~PRU_SUART_CH_CONFIG2_DATALEN_MASK;
    v |= (uint16_t)(dataLen << PRU_SUART_CH_CONFIG2_DATALEN_SHIFT);
    ram_wr16(off, v);

    off = CH_OFF(chNum) + PRU_SUART_CH_TXRXDATA_OFFSET;
    ram_wr32(off, buf_phys);

    off = CH_OFF(chNum) + PRU_SUART_CH_CTRL_OFFSET;
    v = ram_rd16(off);
    v &= ~(PRU_SUART_CH_CTRL_MODE_MASK | PRU_SUART_CH_CTRL_SREQ_MASK);
    v |= (PRU_SUART_CH_CTRL_RX_MODE << PRU_SUART_CH_CTRL_MODE_SHIFT) |
         (PRU_SUART_CH_CTRL_SREQ    << PRU_SUART_CH_CTRL_SREQ_SHIFT);
    ram_wr16(off, v);

    suart_intr_setmask(uartNum, 1, CHN_TXRX_IE_MASK_TIMEOUT);
    arm_to_pru_intr(uartNum);
}

static void suart_clrRxFifo(int s);   /* fwd (read_data calls it on timeout) */

/* pru_softuart_read_data(): pull the completed (or timed-out) RX bytes out of
 * the posted shared-RAM buffer. On timeout it re-arms via clrRxFifo; on a full
 * transfer the PRU ping-pongs the double buffer on its own (lego behaviour). */
static void suart_read_data(int s, uint8_t* out, int32_t maxlen, uint32_t* nread){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2 + 1);   /* RX channel */
    uint32_t off, src, dl, charLen, dataLen, dataRead, i;
    uint16_t status;
    uint8_t* p;

    *nread = 0;
    off = CH_OFF(chNum) + PRU_SUART_CH_TXRXDATA_OFFSET;
    src = ram_rd32(off);

    off = CH_OFF(chNum) + PRU_SUART_CH_CONFIG2_OFFSET;
    dl = ram_rd16(off);
    charLen = (dl & PRU_SUART_CH_CONFIG2_BITPERCHAR_MASK);
    charLen -= 2;                                   /* drop start + stop */
    dataLen = (dl & PRU_SUART_CH_CONFIG2_DATALEN_MASK) >> PRU_SUART_CH_CONFIG2_DATALEN_SHIFT;
    dataLen++;
    if(charLen > 8) dataLen *= 2;

    off = CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET;
    status = ram_rd8(off);

    if(status & CHN_TXRX_STATUS_TIMEOUT){
        off = CH_OFF(chNum) + PRU_SUART_CH_BYTESDONECNTR_OFFSET;
        dataRead = ram_rd8(off);
        if(charLen > 8) dataRead *= 2;
        if(dataRead > dataLen){ dataRead -= dataLen; src += dataLen; }
        suart_clrRxFifo(s);                          /* re-post + re-arm */
    } else {
        dataRead = dataLen;
        if(status & CHN_TXRX_STATUS_CMPLT) src += dataLen;   /* second half */
    }

    if((int32_t)dataRead > maxlen) dataRead = (uint32_t)maxlen;

    /* Reject an unposted/dummy buffer address before translating phys->virt. */
    if(src < EV3_PRU_SUART_FIFO_PHYS ||
       src >= EV3_PRU_SUART_FIFO_PHYS + EV3_SHARED_RAM_SIZE)
        return;

    p = (uint8_t*)((ewokos_addr_t)src - (ewokos_addr_t)EV3_PRU_SUART_FIFO_PHYS
                   + ev3_pru_shared_ram());
    for(i = 0; i < dataRead; i++)
        out[i] = p[i];
    *nread = dataRead;
}

/* pru_softuart_stopReceive(): clear RX busy and deactivate the RX serialiser. */
static void suart_stopReceive(int s){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2 + 1);
    uint32_t off;
    uint16_t status, sr;

    off = CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET;
    status = ram_rd8(off);
    status &= ~CHN_TXRX_STATUS_RDY;
    ram_wr8(off, (uint8_t)status);

    off = CH_OFF(chNum) + PRU_SUART_CH_CTRL_OFFSET;
    sr = ram_rd16(off);
    sr = (sr & PRU_SUART_CH_CTRL_SR_MASK) >> PRU_SUART_CH_CTRL_SR_SHIFT;
    serializer_deactivate(sr);
}

static uint8_t suart_getTxStatus(int s){
    uint16_t chNum = (uint16_t)(UARTNUM(s) * 2 - 2);
    return ram_rd8(CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET);
}
static void suart_clrTxStatus(int s){
    uint16_t chNum = (uint16_t)(UARTNUM(s) * 2 - 2);
    uint32_t off = CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET;
    uint8_t v = ram_rd8(off); v &= ~0x2; ram_wr8(off, v);
}
static uint8_t suart_getRxStatus(int s){
    uint16_t chNum = (uint16_t)(UARTNUM(s) * 2 - 2 + 1);
    return ram_rd8(CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET);
}
static void suart_clrRxStatus(int s){
    uint16_t chNum = (uint16_t)(UARTNUM(s) * 2 - 2 + 1);
    uint32_t off = CH_OFF(chNum) + PRU_SUART_CH_TXRXSTATUS_OFFSET;
    uint8_t v = ram_rd8(off); v &= ~0x3C; ram_wr8(off, v);
}

/* pru_softuart_clrRxFifo(): zero the byte counter and re-post the RX buffer. */
static void suart_clrRxFifo(int s){
    int uartNum = UARTNUM(s);
    uint16_t chNum = (uint16_t)(uartNum * 2 - 2 + 1);
    uint32_t off;
    uint16_t v;

    off = CH_OFF(chNum) + PRU_SUART_CH_BYTESDONECNTR_OFFSET;
    ram_wr8(off, 0);

    off = CH_OFF(chNum) + PRU_SUART_CH_CTRL_OFFSET;
    v = ram_rd16(off);
    v &= ~(PRU_SUART_CH_CTRL_MODE_MASK | PRU_SUART_CH_CTRL_SREQ_MASK);
    v |= (PRU_SUART_CH_CTRL_RX_MODE << PRU_SUART_CH_CTRL_MODE_SHIFT) |
         (PRU_SUART_CH_CTRL_SREQ    << PRU_SUART_CH_CTRL_SREQ_SHIFT);
    ram_wr16(off, v);

    suart_intr_setmask(uartNum, 1, CHN_TXRX_IE_MASK_TIMEOUT);
    arm_to_pru_intr(uartNum);
}

/* pru_softuart_get_isrstatus(): read STATCLRINT1 and acknowledge this SUART's
 * TX/RX system events, reporting which fired. */
static void suart_get_isrstatus(int uartNum, uint16_t* flag){
    uint32_t chNum = (uint32_t)(uartNum * 2 - 2);
    uint32_t isr, regVal;

    *flag = 0;
    isr = ev3_pru_intc_read(PRU_INTC_STATCLRINT1);
    regVal = PRU_SUART0_TX_EVT_BIT << ((uartNum - 1) * 2);
    if(isr & regVal){
        *flag |= PRU_TX_INTR;
        ev3_pru_intc_write(PRU_INTC_STATIDXCLR, chNum + PRU_SUART0_TX_EVT);
    }
    regVal = PRU_SUART0_RX_EVT_BIT << ((uartNum - 1) * 2);
    isr = ev3_pru_intc_read(PRU_INTC_STATCLRINT1);
    if(isr & regVal){
        *flag |= PRU_RX_INTR;
        chNum += 1;
        ev3_pru_intc_write(PRU_INTC_STATIDXCLR, chNum + PRU_SUART0_TX_EVT);
    }
}

/* pru_intr_clr_isrstatus(): clear the ARM->PRU service-request-acked flag. */
static void suart_clr_isrstatus(int uartNum, int rx){
    uint32_t off = PRU_SUART_PRU0_ISR_OFFSET + 1;
    uint8_t v;
    (void)uartNum; (void)rx;   /* TI clears the same byte for TX and RX */
    v = ram_rd8(off); v &= ~0x2; ram_wr8(off, v);
}

/* PRU0 DRAM control-region singletons (pru_set_* / suart_set_pru_id). */
static void set_fifo_timeout(uint16_t t){ ram_wr16(PRU_SUART_PRU0_IDLE_TIMEOUT_OFFSET, t); }
static void set_delay_count(uint32_t freq_mhz){
    uint8_t d = (freq_mhz == 228 || freq_mhz == 186) ? 5 : 3;
    ram_wr8(PRU_SUART_PRU0_DELAY_OFFSET, d);
}
static void set_pru_id0(void){
    uint8_t v = ram_rd8(PRU_SUART_PRU0_ID_ADDR);
    v &= ~SUART_PRU_ID_MASK; v = 0; ram_wr8(PRU_SUART_PRU0_ID_ADDR, v);
}
static void set_rx_tx_mode(uint8_t mode){ ram_wr8(PRU_SUART_PRU0_RX_TX_MODE, mode); }

/* ===================== RX software FIFO ===================== */
static uint32_t rx_count(int s){
    return (_slot[s].rx_head - _slot[s].rx_tail) & PRU_RX_FIFO_MASK;
}
static void rx_push(int s, uint8_t b){
    uint32_t n = (_slot[s].rx_head + 1) & PRU_RX_FIFO_MASK;
    if(n == _slot[s].rx_tail) return;   /* full: drop (overrun counted upstream) */
    _slot[s].rx_fifo[_slot[s].rx_head] = b;
    _slot[s].rx_head = n;
}
static int rx_pop(int s, uint8_t* b){
    if(_slot[s].rx_head == _slot[s].rx_tail) return 0;
    *b = _slot[s].rx_fifo[_slot[s].rx_tail];
    _slot[s].rx_tail = (_slot[s].rx_tail + 1) & PRU_RX_FIFO_MASK;
    return 1;
}

/* Drain one PRU RX event into the software FIFO. get_isrstatus() acknowledges
 * the PRUSS system event, which is what de-asserts PRU_EVTOUTn and therefore
 * clears the ARM AINTC line - so calling this from can_read() inside the
 * existing rx_irq loop both services and clears the interrupt. */
static void rx_service(int s){
    int uartNum = UARTNUM(s);
    uint16_t flag = 0;
    uint8_t tmp[EV3_PRU_SUART_FIFO_LEN + 2];
    uint32_t n = 0, i;
    uint8_t rxstat;

    suart_get_isrstatus(uartNum, &flag);
    if(!(flag & PRU_RX_INTR))
        return;
    suart_clr_isrstatus(uartNum, PRU_RX_INTR);
    rxstat = suart_getRxStatus(s);
    suart_read_data(s, tmp, (int32_t)sizeof(tmp), &n);
    if(!(rxstat & CHN_TXRX_STATUS_ERR)){
        for(i = 0; i < n; i++)
            rx_push(s, tmp[i]);
    }
    suart_clrRxStatus(s);
}

/* ===================== subsystem bring-up =====================
 * pru_softuart_init() for SINGLE_PRU / RX_TX_BOTH, on EwokOS's PRU HAL. */
int ev3_pru_uart_init_subsystem(void){
    ewokos_addr_t sram;
    uint32_t off, z = 0;
    int i;

    if(_subsys_up)
        return 0;

    /* No firmware dropped in yet (see pru_suart_fw.h): leave ports 3/4 inert
     * rather than loading garbage into PRU0. */
    if(ev3_pru_suart_fw_len == 0){
        klog("ev3 pru_uart: PRU_SUART_Emulation.bin absent; ports 3/4 UART off\n");
        return -1;
    }

    if(ev3_pru_init() != 0)
        return -1;

    sram = ev3_pru_shared_ram();
    for(i = 0; i < 2; i++){
        _slot[i].tx_phys = EV3_PRU_SUART_FIFO_PHYS + EV3_PRU_SUART_FIFO_SLOT_SZ * (uint32_t)i;
        _slot[i].rx_phys = _slot[i].tx_phys + EV3_PRU_SUART_FIFO_HALF;
        _slot[i].tx_va   = (uint8_t*)(sram + EV3_PRU_SUART_FIFO_SLOT_SZ * (ewokos_addr_t)i);
        _slot[i].rx_va   = (uint8_t*)(sram + EV3_PRU_SUART_FIFO_SLOT_SZ * (ewokos_addr_t)i + EV3_PRU_SUART_FIFO_HALF);
        _slot[i].rx_head = _slot[i].rx_tail = 0;
        _slot[i].open = 0;
    }

    /* Clock-gate off, then configure McASP as the 230400-base bit clock. */
    ev3_pru_psc_enable();          /* PRUSS  via PSC0 LPSC13 */
    ev3_pru_mcasp_psc_enable();    /* McASP0 via PSC1 LPSC7  */
    mcasp_config_230400();

    /* Hold PRU0 in reset, clear its DRAM (aligned words), load the firmware. */
    ev3_pru_reset(0);
    for(off = 0; off < 0x200; off += 4)
        ram_wr32(off, z);
    if(ev3_pru_load(0, (const uint32_t*)ev3_pru_suart_fw, ev3_pru_suart_fw_len / 4) != 0)
        return -1;

    /* INTC routing, then the firmware control-region singletons, then the
     * per-channel context, then release the core. */
    intc_init();
    set_delay_count(EV3_PRU_CLK_FREQ_MHZ);
    set_pru_id0();
    set_rx_tx_mode(PRU_MODE_RX_TX_BOTH);
    ram_data_init();
    ev3_pru_run(0);

    _subsys_up = 1;
    return 0;
}

/* McASP PINMUX (privileged SYSCFG, via the kernel): AXR1..AXR4 + AHCLKR/AHCLKX
 * to their McASP mode. Idempotent. */
static void pru_pinmux(void){
    ev3_syscfg_write(EV3_PRU_PINMUX_AHBCLK_REG, EV3_PRU_PINMUX_AHBCLK_VAL, EV3_PRU_PINMUX_AHBCLK_MASK);
    ev3_syscfg_write(EV3_PRU_PINMUX_AXR_REG,    EV3_PRU_PINMUX_AXR_VAL,    EV3_PRU_PINMUX_AXR_MASK);
}

/* lego request_port + startup for one channel (RX-driven; TX is synchronous). */
static int port_open(int s){
    int uartNum = UARTNUM(s);

    if(_slot[s].open)
        return 0;

    set_fifo_timeout((uint16_t)EV3_PRU_SUART_FIFO_TIMEOUT);

    suart_intr_clrmask(uartNum, 0, CHN_TXRX_IE_MASK_CMPLT);
    suart_intr_clrmask(uartNum, 1, CHN_TXRX_IE_MASK_BI | CHN_TXRX_IE_MASK_FE |
                                   CHN_TXRX_IE_MASK_CMPLT | CHN_TXRX_IE_MASK_TIMEOUT);

    suart_intr_setmask(uartNum, 1, SUART_GBL_INTR_ERR_MASK);
    suart_intr_setmask(uartNum, 1, CHN_TXRX_IE_MASK_CMPLT | CHN_TXRX_IE_MASK_TIMEOUT);
    suart_intr_setmask(uartNum, 1, CHN_RX_IE_MASK_OVRN);

    pru_to_host_intr_enable(uartNum, 1, 1);          /* RX host event -> EVTOUTn */
    _slot[s].rx_head = _slot[s].rx_tail = 0;
    suart_post_read(s, _slot[s].rx_phys, EV3_PRU_SUART_FIFO_LEN);

    _slot[s].open = 1;
    return 0;
}

static void port_close(int s){
    int uartNum = UARTNUM(s);

    if(!_slot[s].open)
        return;
    suart_stopReceive(s);
    suart_intr_clrmask(uartNum, 1, CHN_TXRX_IE_MASK_BI | CHN_TXRX_IE_MASK_FE |
                                   CHN_TXRX_IE_MASK_CMPLT | CHN_TXRX_IE_MASK_TIMEOUT);
    suart_intr_clrmask(uartNum, 0, CHN_TXRX_IE_MASK_CMPLT);
    pru_to_host_intr_enable(uartNum, 1, 0);
    suart_clrRxFifo(s);
    suart_clrRxStatus(s);
    _slot[s].rx_head = _slot[s].rx_tail = 0;
    _slot[s].open = 0;
}

/* ===================== public transport API ===================== */

int ev3_pru_uart_port_enable(int port){
    int s = pru_slot(port);
    if(s < 0)
        return -1;
    if(ev3_pru_uart_init_subsystem() != 0)
        return -1;
    pru_pinmux();
    return port_open(s);
}

void ev3_pru_uart_port_disable(int port){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return;
    port_close(s);
}

uint32_t ev3_pru_uart_get_irq(int port){
    /* The AINTC line comes from ev3_input_port_uart_irq(); the 16550's IIR-style
     * pending id is not meaningful here (RX is serviced lazily in can_read). */
    (void)port;
    return 0;
}

void ev3_pru_uart_enable_irq(int port, int dir, int en){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return;
    if(dir == EV3_IRQ_RX)
        pru_to_host_intr_enable(UARTNUM(s), 1, en ? 1 : 0);
    /* TX is synchronous - no TX host event to gate. */
}

void ev3_pru_uart_init(int port, int baudrate){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return;
    suart_setdatabits(s, SUART_DATA_BITS8);   /* 8N1 */
    ev3_pru_uart_set_baud(port, baudrate);
    _slot[s].rx_head = _slot[s].rx_tail = 0;
}

void ev3_pru_uart_set_baud(int port, int baudrate){
    int s = pru_slot(port);
    uint32_t div;
    if(s < 0 || !_subsys_up)
        return;
    if(baudrate < EV3_PRU_SUART_MIN_BAUD) baudrate = EV3_PRU_SUART_MIN_BAUD;
    if(baudrate > EV3_PRU_SUART_MAX_BAUD) baudrate = EV3_PRU_SUART_MAX_BAUD;  /* 230400 cap */
    div = EV3_PRU_SUART_BASE_BAUD / (uint32_t)baudrate;
    if(div == 0) div = 1;
    if(div > EV3_PRU_SUART_DIV_MAX) div = EV3_PRU_SUART_DIV_MAX;
    suart_setbaud(s, (uint16_t)div);
}

void ev3_pru_uart_flush_rx(int port){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return;
    _slot[s].rx_head = _slot[s].rx_tail = 0;
    suart_clrRxFifo(s);
    suart_clrRxStatus(s);
}

int ev3_pru_uart_can_read(int port){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return 0;
    if(rx_count(s) == 0)
        rx_service(s);
    return rx_count(s) > 0;
}

char ev3_pru_uart_getc(int port){
    int s = pru_slot(port);
    uint8_t b = 0;
    if(s < 0 || !_subsys_up)
        return 0;
    if(rx_count(s) == 0)
        rx_service(s);
    rx_pop(s, &b);
    return (char)b;
}

int ev3_pru_uart_can_write(int port){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return 0;
    return (suart_getTxStatus(s) & CHN_TXRX_STATUS_RDY) ? 0 : 1;
}

int ev3_pru_uart_tx_empty(int port){
    int s = pru_slot(port);
    if(s < 0 || !_subsys_up)
        return 1;
    return (suart_getTxStatus(s) & CHN_TXRX_STATUS_RDY) ? 0 : 1;
}

void ev3_pru_uart_putc(int port, char ch){
    int s = pru_slot(port);
    uint32_t guard = 0;
    if(s < 0 || !_subsys_up)
        return;
    /* Wait for any prior single-byte transfer to leave, then re-post. Sensor TX
     * is only a few command bytes, so a bounded synchronous wait is fine and
     * keeps the TX path interrupt-free. */
    while((suart_getTxStatus(s) & CHN_TXRX_STATUS_RDY) && guard++ < 100000)
        ;
    suart_clrTxStatus(s);
    *(_slot[s].tx_va) = (uint8_t)ch;
    suart_write(s, _slot[s].tx_phys, 1);
}
