#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/proc.h>
#include <sysinfo.h>

#include "mmc.h"

#define WATCHDOG_COUNT      (1000000)
#define EV3_SD_MAX_BLOCKS   128

/*
 * EDMA bring-up level (no console on this box, so bisect by rebuilding):
 *   0  pure PIO, EDMA code never touched
 *   1  edma_init only (dma_alloc, PSC, CC registers), transfers stay PIO
 *   2  edma_init + selftest transfers, then EDMA is switched off again
 *   3  full: EDMA used for every qualifying read
 */
#ifndef EV3_SD_USE_EDMA
#define EV3_SD_USE_EDMA     3
#endif
#define EV3_SD_INPUT_CLK    100000000U  /* AM1808 MMC_CLKIN from PLL0 */
#define MMC_CMD6_CHECK_HS   0x00FFFFF1U
#define MMC_CMD6_SWITCH_HS  0x80FFFFF1U

#define get_val(addr)       (*(volatile uint32_t*)(addr))
#define set_val(addr, val)  (*(volatile uint32_t*)(addr) = (val))
#define set_bit(addr, val)  set_val((addr), (get_val(addr) | (val)))
#define clear_bit(addr, val)    set_val((addr), (get_val(addr) & ~(val)))

/* ---- AM1808 EDMA3 CC definitions (layout as in Linux drivers/dma/edma.c) ---- */
#define EDMA_TPCC_BASE      0x01C00000U
/* global registers */
#define EDMA_CCCFG          0x0004
#define EDMA_CCCFG_CHMAP    (1U << 24)      /* CHMAP_EXIST */
#define EDMA_DCHMAP(n)      (0x0100 + (n)*4)
#define EDMA_DMAQNUM(n)     (0x0240 + (n)*4)
#define EDMA_EMR            0x0300
#define EDMA_EMCR           0x0308
#define EDMA_CCERR          0x0318
#define EDMA_CCERRCLR       0x031C
/* global channel registers (0x1000), same layout as the shadow regions */
#define EDMA_ER             0x1000
#define EDMA_ECR            0x1008
#define EDMA_ESR            0x1010
#define EDMA_EER            0x1020          /* read-only: use EESR/EECR */
#define EDMA_EECR           0x1028
#define EDMA_EESR           0x1030
#define EDMA_SER            0x1038
#define EDMA_SECR           0x1040
#define EDMA_IER            0x1050          /* read-only: use IESR/IECR */
#define EDMA_IECR           0x1058
#define EDMA_IESR           0x1060
#define EDMA_IPR            0x1068
#define EDMA_ICR            0x1070
#define EDMA_IEVAL          0x1078
#define EDMA_PARAM(n)       (0x4000 + (n)*32)

/* MMCSD0 EDMA events (AM1808/DA850) */
#define EDMA_EVT_MMCSD_RX   16
#define EDMA_EVT_MMCSD_TX   17
#define EDMA_CH_RX          16
#define EDMA_CH_TX          17

/* PaRAM OPT bits (EDMA3 TPCC, see SPRUH77 / Linux edma.h) */
#define EDMA_OPT_SYNCDIM    (1U << 2)   /* AB-sync */
#define EDMA_OPT_TCC(n)     (((uint32_t)(n) & 0x3F) << 12)
#define EDMA_OPT_TCINTEN    (1U << 20)  /* transfer complete interrupt */

/* PSC0: TPCC = LPSC0, TPTC0 = LPSC1, TPTC1 = LPSC2 */
#define PSC0_BASE           0x01C10000U
#define PSC_PTCMD           0x120
#define PSC_PTSTAT          0x128
#define PSC_MDSTAT(n)       (0x800 + (n)*4)
#define PSC_MDCTL(n)        (0xA00 + (n)*4)

/* Physical addresses of MMC FIFO registers */
#define MMC_PHY_MMCDRR      0x01C40028U
#define MMC_PHY_MMCDXR      0x01C4002CU

/*
 * DMA FIFO threshold: 32 bytes = 8 words (Linux rw_threshold).
 * On this controller (MMC_CTLR_VERSION_2) FIFOLEV=0 selects the 32-byte
 * level, FIFOLEV=1 the 64-byte level; the PIO path below uses 64 bytes.
 */
#define EDMA_FIFO_THRESHOLD 32U
#define EDMA_FIFO_WORDS     (EDMA_FIFO_THRESHOLD / 4U)
#define EDMA_FIFOLEV        0U

/* self-test: sectors compared between PIO and EDMA at init (max CMD18 chunk) */
#define EDMA_SELFTEST_SECTORS EV3_SD_MAX_BLOCKS

/* Max EDMA transfer: CCNT is 16-bit */
#define EDMA_MAX_BYTES      (65535U * EDMA_FIFO_THRESHOLD)

#define EDMA_DMA_BUF_SIZE   (EV3_SD_MAX_BLOCKS * 512U)

static uint8_t* _edma_buf = 0;          /* virtual addr of DMA bounce buffer */
static uint32_t _edma_buf_phy = 0;      /* physical addr of DMA bounce buffer */
static volatile uint32_t* _edma_base = 0; /* virtual addr of EDMA TPCC regs */
static int _edma_ready = 0;

/* Busy bit wait loop for MMCST1 */
static int dmmc_busy_wait(volatile struct davinci_mmc_regs *regs)
{
    uint32_t  wdog = WATCHDOG_COUNT;

    while (--wdog && (get_val(&regs->mmcst1) & MMCST1_BUSY));

    if (wdog == 0)
        return -1;

    return 0;
}

/*
 * Linux mmc_davinci_reset_ctrl(1)/(0): pull the command and data state
 * machines out of whatever an aborted transfer left them in. Bus width,
 * clock and the rest of MMCCTL are untouched, so the card session survives.
 */
static void dmmc_reset_ctrl(volatile struct davinci_mmc_regs *regs)
{
    volatile int spin;
    set_bit(&regs->mmcctl, MMCCTL_CMDRST | MMCCTL_DATRST);
    for (spin = 0; spin < 1000; spin++) ;
    set_val(&regs->mmcfifoctl, MMCFIFOCTL_FIFORST);
    clear_bit(&regs->mmcctl, MMCCTL_CMDRST | MMCCTL_DATRST);
    for (spin = 0; spin < 1000; spin++) ;
}

/* ---- EDMA3 helpers ---- */

static inline uint32_t edma_reg(uint32_t offset) {
    return _edma_base[offset >> 2];
}

static inline void edma_set(uint32_t offset, uint32_t val) {
    _edma_base[offset >> 2] = val;
}

/*
 * Write a PaRAM set (32 bytes = 8 words):
 *  0x00 OPT
 *  0x04 SRC
 *  0x08 A_B_CNT       BCNT[31:16] | ACNT[15:0]
 *  0x0C DST
 *  0x10 SRC_DST_BIDX  DSTBIDX[31:16] | SRCBIDX[15:0]
 *  0x14 LINK_BCNTRLD  BCNTRLD[31:16] | LINK[15:0]
 *  0x18 SRC_DST_CIDX  DSTCIDX[31:16] | SRCCIDX[15:0]
 *  0x1C CCNT          CCNT[15:0]
 */
static void edma_write_param(int ch, uint32_t opt, uint32_t src, uint32_t dst,
        uint32_t acnt, uint32_t bcnt, uint32_t ccnt,
        int32_t src_bidx, int32_t dst_bidx,
        int32_t src_cidx, int32_t dst_cidx,
        uint32_t link, uint32_t bcntrld)
{
    uint32_t base = EDMA_PARAM(ch);
    _edma_base[(base + 0x00) >> 2] = opt;
    _edma_base[(base + 0x04) >> 2] = src;
    _edma_base[(base + 0x08) >> 2] = ((bcnt & 0xFFFF) << 16) | (acnt & 0xFFFF);
    _edma_base[(base + 0x0C) >> 2] = dst;
    _edma_base[(base + 0x10) >> 2] = (((uint32_t)dst_bidx & 0xFFFF) << 16) | ((uint32_t)src_bidx & 0xFFFF);
    _edma_base[(base + 0x14) >> 2] = ((bcntrld & 0xFFFF) << 16) | (link & 0xFFFF);
    _edma_base[(base + 0x18) >> 2] = (((uint32_t)dst_cidx & 0xFFFF) << 16) | ((uint32_t)src_cidx & 0xFFFF);
    _edma_base[(base + 0x1C) >> 2] = ccnt & 0xFFFF;
}

/* Enable a PSC0 module (EDMA CC/TC clocks); bootloader usually did already */
static void psc0_enable(int module) {
    volatile uint32_t* mdctl  = (volatile uint32_t*)(_mmio_base + PSC0_BASE + PSC_MDCTL(module));
    volatile uint32_t* mdstat = (volatile uint32_t*)(_mmio_base + PSC0_BASE + PSC_MDSTAT(module));
    volatile uint32_t* ptcmd  = (volatile uint32_t*)(_mmio_base + PSC0_BASE + PSC_PTCMD);
    volatile uint32_t* ptstat = (volatile uint32_t*)(_mmio_base + PSC0_BASE + PSC_PTSTAT);
    int t;

    if ((*mdstat & 0x1f) == 0x3)
        return;
    t = 100000; while (*ptstat && --t > 0) ;
    *mdctl = (*mdctl & ~0x1f) | 0x3;   /* NEXT = ENABLE */
    *ptcmd = 0x1;
    t = 100000; while (*ptstat && --t > 0) ;
    t = 100000; while (((*mdstat & 0x1f) != 0x3) && --t > 0) ;
}

/* Equivalent of Linux edma_stop(): disable event, clear pending/missed/secondary/irq */
static void edma_stop(int ch) {
    uint32_t mask = 1U << ch;
    edma_set(EDMA_EECR, mask);
    edma_set(EDMA_ECR,  mask);
    edma_set(EDMA_SECR, mask);
    edma_set(EDMA_EMCR, mask);
    edma_set(EDMA_ICR,  mask);
}

/* Equivalent of Linux edma_start() for a hw-triggered channel */
static void edma_start(int ch) {
    uint32_t mask = 1U << ch;
    edma_set(EDMA_ECR,  mask);
    edma_set(EDMA_EMCR, mask);
    edma_set(EDMA_SECR, mask);
    edma_set(EDMA_ICR,  mask);
    edma_set(EDMA_EESR, mask);  /* EER is read-only; enable via the SET register */
}

static void edma_init(void) {
    /* EDMA registers are in the same MMIO window as MMC */
    _edma_base = (volatile uint32_t*)(_mmio_base + EDMA_TPCC_BASE);

    /* Allocate DMA bounce buffer (non-cacheable, known physical addr) */
    _edma_buf = (uint8_t*)dma_alloc(0, EDMA_DMA_BUF_SIZE);
    if(_edma_buf == 0)
        return;
    _edma_buf_phy = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)_edma_buf);
    if(_edma_buf_phy == 0)
        return;

    psc0_enable(0); /* TPCC  */
    psc0_enable(1); /* TPTC0 */
    psc0_enable(2); /* TPTC1 */

    /* Channel -> PaRAM mapping only exists when CCCFG.CHMAP_EXIST is set;
       value is the PaRAM number << 5 (Linux edma_set_chmap) */
    if (edma_reg(EDMA_CCCFG) & EDMA_CCCFG_CHMAP) {
        edma_set(EDMA_DCHMAP(EDMA_CH_RX), EDMA_CH_RX << 5);
        edma_set(EDMA_DCHMAP(EDMA_CH_TX), EDMA_CH_TX << 5);
    }

    /* Channels 16..23 -> transfer queue 0 */
    edma_set(EDMA_DMAQNUM(2), 0);

    /* No completion interrupts: we poll IPR */
    edma_set(EDMA_IECR, (1U << EDMA_CH_RX) | (1U << EDMA_CH_TX));
    edma_stop(EDMA_CH_RX);
    edma_stop(EDMA_CH_TX);
    edma_set(EDMA_CCERRCLR, (1U << 16) | (1U << 1) | (1U << 0));

    _edma_ready = 1;
}

/* Wait for the transfer-complete flag (IPR) of the channel's final TR */
static int edma_wait_done(int ch) {
    uint32_t mask = 1U << ch;
    uint32_t wdog = WATCHDOG_COUNT;
    while (--wdog) {
        if (edma_reg(EDMA_IPR) & mask)
            return 0;
        if (edma_reg(EDMA_EMR) & mask)
            return -1;
    }
    return -1;
}

/* Set up and start EDMA for a read (MMCSD RX: MMCDRR -> buffer) */
static void edma_start_rx(uint32_t total_bytes) {
    uint32_t ccnt = total_bytes / EDMA_FIFO_THRESHOLD;

    edma_write_param(EDMA_CH_RX,
            EDMA_OPT_SYNCDIM | EDMA_OPT_TCINTEN | EDMA_OPT_TCC(EDMA_CH_RX),
            MMC_PHY_MMCDRR,         /* src: MMC data receive register */
            _edma_buf_phy,          /* dst: DMA bounce buffer */
            4,                      /* acnt: 4 bytes per element */
            EDMA_FIFO_WORDS,        /* bcnt: 8 words = 32 bytes per frame */
            ccnt,                   /* ccnt: number of frames */
            0,                      /* src_bidx: don't advance src */
            4,                      /* dst_bidx: advance dst by 4 */
            0,                      /* src_cidx: don't advance src */
            (int32_t)EDMA_FIFO_THRESHOLD, /* dst_cidx: advance by 32 */
            0xFFFF,                 /* link: null */
            0xFFFF);                /* bcntrld (Linux: link_bcntrld = 0xffffffff) */

    edma_start(EDMA_CH_RX);
}

/* Status bit wait loop for MMCST1 */
static int
dmmc_wait_fifo_status(volatile struct davinci_mmc_regs *regs, unsigned int status)
{
    int wdog = WATCHDOG_COUNT;

    while (--wdog && ((get_val(&regs->mmcst1) & status) != status));

    if (wdog == 0)
        return -1;

    return 0;
}

/* Status bit wait loop for MMCST0 - Checks for error bits as well */
static int dmmc_check_status(volatile struct davinci_mmc_regs *regs,
        unsigned int *cur_st, unsigned int st_ready, unsigned int st_error)
{
    int wdog = WATCHDOG_COUNT;
    unsigned int mmcstatus = *cur_st;

    while (wdog--) {
        if (mmcstatus & st_ready) {
            *cur_st = mmcstatus;
            mmcstatus = get_val(&regs->mmcst1);
            return 0;
        } else if (mmcstatus & st_error) {
            if (mmcstatus & MMCST0_TOUTRS)
                return -2;
            printf("[ ST0 ERROR %x]\n", mmcstatus);
            /*
             * Ignore CRC errors as some MMC cards fail to
             * initialize on DM365-EVM on the SD1 slot
             */
            if (mmcstatus & MMCST0_CRCRS)
                return 0;
            return -1;
        }
        mmcstatus = get_val(&regs->mmcst0);
    }

    printf("Status %x Timeout ST0:%x ST1:%lx\n", st_ready, mmcstatus,
            get_val(&regs->mmcst1));
    return -1;
}

static int
davinci_mmc_send_cmd(struct davinci_mmc_regs *regs, struct mmc_cmd *cmd, struct mmc_data *data)
{
    unsigned int mmcstatus, status_rdy, status_err;
    unsigned cmddata, bytes_left = 0;
    unsigned int i, fifo_words, fifo_bytes, err;
    char *data_buf = NULL;
    int use_edma = 0;

    /* Clear status registers */
    mmcstatus = get_val(&regs->mmcst0);
    fifo_words = 16;
    fifo_bytes = fifo_words << 2;
    /* Wait for any previous busy signal to be cleared */
    dmmc_busy_wait(regs);

    cmddata = cmd->cmdidx;
    cmddata |= MMCCMD_PPLEN;

    /* Send init clock for CMD0 */
    if (cmd->cmdidx == MMC_CMD_GO_IDLE_STATE)
        cmddata |= MMCCMD_INITCK;

    switch (cmd->resp_type) {
    case MMC_RSP_R1b:
        cmddata |= MMCCMD_BSYEXP;
        /* Fall-through */
    case MMC_RSP_R1:    /* R1, R1b, R5, R6, R7 */
        cmddata |= MMCCMD_RSPFMT_R1567;
        break;
    case MMC_RSP_R2:
        cmddata |= MMCCMD_RSPFMT_R2;
        break;
    case MMC_RSP_R3: /* R3, R4 */
        cmddata |= MMCCMD_RSPFMT_R3;
        break;
    }

    set_val(&regs->mmcim, 0);

    if (data) {
        uint32_t total_bytes = data->blocksize * data->blocks;
        bytes_left = total_bytes;

        /* EDMA is used for reads only: the read path is verified against
           PIO at init (edma_selftest), a wrong write would silently damage
           the card, and writes are rare on this box anyway. */
        use_edma = _edma_ready &&
                   data->flags == MMC_DATA_READ &&
                   total_bytes >= EDMA_FIFO_THRESHOLD &&
                   (total_bytes % EDMA_FIFO_THRESHOLD) == 0 &&
                   total_bytes <= EDMA_DMA_BUF_SIZE;

        /* FIFO level: 64 bytes for PIO (fifo_words), 32 bytes for EDMA
           (davinci_mmc.c: fifo_lev = 0 for rw_threshold 32 on VERSION_2) */
        uint32_t fifo_lev = use_edma ? EDMA_FIFOLEV : MMCFIFOCTL_FIFOLEV;

        /* Reset FIFO */
        set_val(&regs->mmcfifoctl, (fifo_lev | MMCFIFOCTL_FIFORST));

        /* keep DMATRIG as the proven PIO path did (harmless without EDMA events) */
        cmddata |= MMCCMD_DMATRIG;

        cmddata |= MMCCMD_WDATX;
        if (data->flags == MMC_DATA_READ) {
            set_val(&regs->mmcfifoctl, fifo_lev);
        } else if (data->flags == MMC_DATA_WRITE) {
            set_val(&regs->mmcfifoctl, (fifo_lev | MMCFIFOCTL_FIFODIR));
            cmddata |= MMCCMD_DTRW;
        }

        set_val(&regs->mmctod, 0xFFFF);
        set_val(&regs->mmcnblk, (data->blocks & MMCNBLK_NBLK_MASK));
        set_val(&regs->mmcblen, (data->blocksize & MMCBLEN_BLEN_MASK));

        if (use_edma) {
            edma_start_rx(total_bytes);
            bytes_left = 0; /* EDMA handles all data */
        } else if (data->flags == MMC_DATA_WRITE) {
            unsigned int val;
            data_buf = (char *)data->un.src;
            /* For write, fill FIFO with data before issue of CMD */
            for (i = 0; (i < fifo_words) && bytes_left; i++) {
                memcpy((char *)&val, data_buf, 4);
                set_val(&regs->mmcdxr, val);
                data_buf += 4;
                bytes_left -= 4;
            }
        }
    } else {
        set_val(&regs->mmcblen, 0);
        set_val(&regs->mmcnblk, 0);
    }

    set_val(&regs->mmctor, 0x1FFF);

    /* Send the command */
    set_val(&regs->mmcarghl, cmd->cmdarg);
    set_val(&regs->mmccmd, cmddata);

    status_rdy = MMCST0_RSPDNE;
    status_err = (MMCST0_TOUTRS | MMCST0_TOUTRD |
            MMCST0_CRCWR | MMCST0_CRCRD);
    if (cmd->resp_type & MMC_RSP_CRC)
        status_err |= MMCST0_CRCRS;

    mmcstatus = get_val(&regs->mmcst0);
    err = dmmc_check_status(regs, &mmcstatus, status_rdy, status_err);
    if (err) {
        if (use_edma)
            edma_stop(EDMA_CH_RX);
        return err;
    }

    /* For R1b wait for busy done */
    if (cmd->resp_type == MMC_RSP_R1b)
        dmmc_busy_wait(regs);

    /* Collect response from controller for specific commands */
    if (mmcstatus & MMCST0_RSPDNE) {
        if (cmd->resp_type & MMC_RSP_136) {
            cmd->response[0] = get_val(&regs->mmcrsp67);
            cmd->response[1] = get_val(&regs->mmcrsp45);
            cmd->response[2] = get_val(&regs->mmcrsp23);
            cmd->response[3] = get_val(&regs->mmcrsp01);
        } else if (cmd->resp_type & MMC_RSP_PRESENT) {
            cmd->response[0] = get_val(&regs->mmcrsp67);
        }
    }

    if (data == NULL)
        return 0;

    /* --- EDMA read path: wait for DATDNE, then for the DMA to drain --- */
    if (use_edma) {
        int ch = EDMA_CH_RX;
        uint32_t wdog = WATCHDOG_COUNT;
        int dma_err;
        status_err = MMCST0_TOUTRD | MMCST0_CRCRD;

        /* MMCST0 is clear-on-read: DATDNE may already sit in the word that
           carried RSPDNE (short transfers), don't drop it */
        while (!(mmcstatus & (MMCST0_DATDNE | status_err)) && --wdog)
            mmcstatus = get_val(&regs->mmcst0);

        if (wdog == 0 || (mmcstatus & status_err)) {
            /* card side never finished or errored: the FIFO/data state
               machine is mid-transfer, reset it so the PIO fallback and the
               following CMD12 start from a clean controller */
            edma_stop(ch);
            dmmc_reset_ctrl(regs);
            if (wdog == 0) {
                _edma_ready = 0;
                printf("ev3_sd: edma rx stalled, falling back to pio\n");
            }
            return -1;
        }

        /* DATDNE only means the card side finished; the last FIFO burst
           may still be in flight, so wait for the EDMA completion flag */
        dma_err = edma_wait_done(ch);
        edma_stop(ch);

        if (dma_err) {
            /* EDMA misbehaving on this board: permanently fall back to PIO */
            _edma_ready = 0;
            dmmc_reset_ctrl(regs);
            printf("ev3_sd: edma rx failed, falling back to pio\n");
            return -1;
        }

        memcpy(data->un.dest, _edma_buf, data->blocksize * data->blocks);
        return 0;
    }

    /* --- PIO data path (fallback for small/control transfers) --- */
    if (data->flags == MMC_DATA_READ) {
        status_rdy = MMCST0_DRRDY | MMCST0_DATDNE;
        status_err = MMCST0_TOUTRD | MMCST0_CRCRD;
        data_buf = data->un.dest;
    } else {
        status_rdy = MMCST0_DXRDY | MMCST0_DATDNE;
        status_err = MMCST0_CRCWR;
    }

    /* Wait until all of the blocks are transferred */
    while (bytes_left) {
        err = dmmc_check_status(regs, &mmcstatus, status_rdy,
                status_err);
        if (err)
            return err;

        if (data->flags == MMC_DATA_READ) {
            if (bytes_left > fifo_bytes)
                dmmc_wait_fifo_status(regs, 0x4a);
            else if (bytes_left == fifo_bytes) {
                dmmc_wait_fifo_status(regs, 0x40);
                if (cmd->cmdidx == MMC_CMD_SEND_EXT_CSD)
                    proc_usleep(0);
            }

            for (i = 0; bytes_left && (i < fifo_words); i++) {
                cmddata = get_val(&regs->mmcdrr);
                memcpy(data_buf, (char *)&cmddata, 4);
                data_buf += 4;
                bytes_left -= 4;
            }
        } else {
            dmmc_wait_fifo_status(regs, MMCST1_FIFOEMP);
            for (i = 0; bytes_left && (i < fifo_words); i++) {
                memcpy((char *)&cmddata, data_buf, 4);
                set_val(&regs->mmcdxr, cmddata);
                data_buf += 4;
                bytes_left -= 4;
            }
            dmmc_busy_wait(regs);
        }
    }

    err = dmmc_check_status(regs, &mmcstatus, MMCST0_DATDNE, status_err);
    if (err)
        return err;

    return 0;
}

static int ev3_sd_read_raw(volatile struct davinci_mmc_regs *regs,
        int32_t sector, void* buf, uint32_t count) {
    struct mmc_cmd cmd;
    struct mmc_data data;
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmdidx = (count == 1) ? MMC_CMD_READ_SINGLE_BLOCK : MMC_CMD_READ_MULTIPLE_BLOCK;
    cmd.cmdarg = sector;
    cmd.resp_type = MMC_RSP_R1;
    data.un.dest = (char*)buf;
    data.blocks = count;
    data.blocksize = 512;
    data.flags = MMC_DATA_READ;

    ret = davinci_mmc_send_cmd((void*)regs, &cmd, &data);

    if (count > 1) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        cmd.resp_type = MMC_RSP_R1;
        davinci_mmc_send_cmd((void*)regs, &cmd, NULL);
    }
    return ret;
}

/*
 * Verify the EDMA read path against PIO before trusting it: a full-size
 * multi-block read (max CMD18 chunk), a single-block read and a second
 * (re-armed) multi-block read must match byte-for-byte, otherwise EDMA is
 * disabled for good.
 */
static void edma_selftest(volatile struct davinci_mmc_regs *regs) {
    const uint32_t bytes = EDMA_SELFTEST_SECTORS * 512U;
    uint8_t* pio_buf;
    uint8_t* dma_buf;
    int ok = 0;

    if (!_edma_ready)
        return;

    pio_buf = (uint8_t*)malloc(bytes);
    dma_buf = (uint8_t*)malloc(bytes);
    if (pio_buf == 0 || dma_buf == 0) {
        _edma_ready = 0;
        goto out;
    }

    _edma_ready = 0;
    if (ev3_sd_read_raw(regs, 0, pio_buf, EDMA_SELFTEST_SECTORS) != 0)
        goto out;                   /* card itself is unhappy: stay on PIO */
    _edma_ready = 1;

    memset(dma_buf, 0xA5, bytes);
    if (ev3_sd_read_raw(regs, 0, dma_buf, EDMA_SELFTEST_SECTORS) != 0 ||
            memcmp(pio_buf, dma_buf, bytes) != 0)
        goto out;

    memset(dma_buf, 0x5A, 512);
    if (ev3_sd_read_raw(regs, 1, dma_buf, 1) != 0 ||
            memcmp(pio_buf + 512, dma_buf, 512) != 0)
        goto out;

    memset(dma_buf, 0xA5, 8 * 512);
    if (ev3_sd_read_raw(regs, 0, dma_buf, 8) != 0 ||
            memcmp(pio_buf, dma_buf, 8 * 512) != 0)
        goto out;
    ok = 1;

out:
    if (!ok) {
        _edma_ready = 0;
        printf("ev3_sd: edma selftest failed, using pio\n");
    }
    if (pio_buf) free(pio_buf);
    if (dma_buf) free(dma_buf);
}

/* 4-bit bus + CMD6 High-Speed negotiation; all transfers here are PIO */
static void ev3_sd_setup_bus(volatile struct davinci_mmc_regs *regs) {
    struct mmc_cmd cmd;
    struct mmc_data data;
    uint8_t switch_status[64];
    uint8_t verify_buf[512];
    uint32_t saved_clkrt;

    memset(&cmd, 0, sizeof(cmd));

    /* --- 4-bit bus width (ACMD6 = CMD55 + ACMD6) --- */
    cmd.cmdidx = MMC_CMD_APP_CMD;
    cmd.cmdarg = 0;
    cmd.resp_type = MMC_RSP_R1;
    davinci_mmc_send_cmd((void*)regs, &cmd, NULL);

    cmd.cmdidx = SD_CMD_APP_SET_BUS_WIDTH;
    cmd.cmdarg = 2;
    cmd.resp_type = MMC_RSP_R1;
    if(davinci_mmc_send_cmd((void*)regs, &cmd, NULL) == 0) {
        set_bit(&regs->mmcctl, MMCCTL_WIDTH_4_BIT);
    }

    /* --- High-Speed mode (50MHz) via CMD6 --- */
    /* Step 1: CMD6 CHECK - read 64-byte switch status */
    cmd.cmdidx = SD_CMD_SWITCH_FUNC;
    cmd.cmdarg = MMC_CMD6_CHECK_HS;
    cmd.resp_type = MMC_RSP_R1;
    data.un.dest = (char*)switch_status;
    data.blocks = 1;
    data.blocksize = 64;
    data.flags = MMC_DATA_READ;

    if(davinci_mmc_send_cmd((void*)regs, &cmd, &data) != 0)
        return;  /* HS check failed, stay at default speed */

    /* Byte 13 bit 1 of switch status = HS supported in group 1 */
    if(!(switch_status[13] & 0x02))
        return;  /* card does not support HS */

    /* Step 2: CMD6 SWITCH - actually switch card to HS */
    cmd.cmdidx = SD_CMD_SWITCH_FUNC;
    cmd.cmdarg = MMC_CMD6_SWITCH_HS;
    cmd.resp_type = MMC_RSP_R1;
    if(davinci_mmc_send_cmd((void*)regs, &cmd, NULL) != 0)
        return;  /* switch failed, card stays default speed */

    /* Step 3: Set host clock to 50MHz (CLKRT=0: 100/(2*1)=50MHz) */
    saved_clkrt = get_val(&regs->mmcclk);
    set_val(&regs->mmcclk, MMCCLK_CLKEN | 0);  /* CLKRT=0 */

    /* Step 4: Verify with a test read of sector 0 */
    if(ev3_sd_read_raw(regs, 0, verify_buf, 1) != 0 ||
       verify_buf[510] != 0x55 || verify_buf[511] != 0xAA) {
        /* HS verification failed: revert card and host */
        cmd.cmdidx = SD_CMD_SWITCH_FUNC;
        cmd.cmdarg = 0x80FFFFF0U;  /* switch back to default */
        cmd.resp_type = MMC_RSP_R1;
        davinci_mmc_send_cmd((void*)regs, &cmd, NULL);
        set_val(&regs->mmcclk, saved_clkrt);
    }
    /* else: HS mode active: 50MHz x 4-bit = ~25MB/s theoretical bus */
}

int32_t ev3_sd_init(void) {
    _mmio_base = mmio_map();

    volatile struct davinci_mmc_regs *regs =
        (volatile struct davinci_mmc_regs*)(_mmio_base + MMC_BASE);

    /* bus width / high speed first, using the proven PIO path only */
    ev3_sd_setup_bus(regs);

    /* then bring up EDMA and only keep it if it reproduces PIO results */
#if EV3_SD_USE_EDMA >= 1
    edma_init();
#endif
#if EV3_SD_USE_EDMA >= 2
    edma_selftest(regs);
#endif
#if EV3_SD_USE_EDMA < 3
    _edma_ready = 0;
#endif
    return 0;
}

int32_t ev3_sd_read_sector(int32_t sector, void* buf) {
    struct mmc_cmd cmd;
    struct mmc_data data;

    cmd.cmdidx = MMC_CMD_READ_SINGLE_BLOCK;
    cmd.cmdarg = sector;
    cmd.resp_type = MMC_RSP_R1;

    data.un.dest = (char*)buf;
    data.blocks = 1;
    data.blocksize = 512;
    data.flags = MMC_DATA_READ;

    if (davinci_mmc_send_cmd((void*)(_mmio_base + MMC_BASE), &cmd, &data))
        return -1;

    return 0;
}

int32_t ev3_sd_read_sectors(int32_t sector, void* buf, uint32_t count) {
    volatile struct davinci_mmc_regs *regs =
        (volatile struct davinci_mmc_regs*)(_mmio_base + MMC_BASE);
    char *dst = (char*)buf;

    if(count == 0)
        return 0;
    if(count == 1)
        return ev3_sd_read_sector(sector, buf);

    while(count > 0) {
        uint32_t chunk = (count > EV3_SD_MAX_BLOCKS) ? EV3_SD_MAX_BLOCKS : count;
        struct mmc_cmd cmd;
        struct mmc_data data;

        cmd.cmdidx = MMC_CMD_READ_MULTIPLE_BLOCK;
        cmd.cmdarg = sector;
        cmd.resp_type = MMC_RSP_R1;

        data.un.dest = dst;
        data.blocks = chunk;
        data.blocksize = 512;
        data.flags = MMC_DATA_READ;

        if(davinci_mmc_send_cmd((void*)regs, &cmd, &data)) {
            /* multi-block failed, try single-block fallback */
            if(ev3_sd_read_sector(sector, dst) != 0)
                return -1;
            dst += 512;
            sector++;
            count--;
            continue;
        }

        /* CMD12: stop transmission (R1, no busy for read-stop) */
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        cmd.cmdarg = 0;
        cmd.resp_type = MMC_RSP_R1;
        davinci_mmc_send_cmd((void*)regs, &cmd, NULL);

        dst += chunk * 512;
        sector += (int32_t)chunk;
        count -= chunk;
    }
    return 0;
}

int32_t ev3_sd_write_sector(int32_t sector, const void* buf) {
    struct mmc_cmd cmd;
    struct mmc_data data;

    cmd.cmdidx = MMC_CMD_WRITE_SINGLE_BLOCK;
    cmd.cmdarg = sector;
    cmd.resp_type = MMC_RSP_R1;

    data.un.src = (const char*)buf;
    data.blocks = 1;
    data.blocksize = 512;
    data.flags = MMC_DATA_WRITE;

    if(davinci_mmc_send_cmd((void*)(_mmio_base + MMC_BASE), &cmd, &data))
        return -1;

    return 0;
}

int32_t ev3_sd_write_sectors(int32_t sector, const void* buf, uint32_t count) {
    volatile struct davinci_mmc_regs *regs =
        (volatile struct davinci_mmc_regs*)(_mmio_base + MMC_BASE);
    const char *src = (const char*)buf;

    if(count == 0)
        return 0;
    if(count == 1)
        return ev3_sd_write_sector(sector, buf);

    while(count > 0) {
        uint32_t chunk = (count > EV3_SD_MAX_BLOCKS) ? EV3_SD_MAX_BLOCKS : count;
        struct mmc_cmd cmd;
        struct mmc_data data;

        cmd.cmdidx = MMC_CMD_WRITE_MULTIPLE_BLOCK;
        cmd.cmdarg = sector;
        cmd.resp_type = MMC_RSP_R1;

        data.un.src = src;
        data.blocks = chunk;
        data.blocksize = 512;
        data.flags = MMC_DATA_WRITE;

        if(davinci_mmc_send_cmd((void*)regs, &cmd, &data)) {
            /* multi-block write failed, single-block fallback */
            if(ev3_sd_write_sector(sector, src) != 0)
                return -1;
            src += 512;
            sector++;
            count--;
            continue;
        }

        /* CMD12: stop transmission (R1b for write-stop: card may hold busy) */
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        cmd.cmdarg = 0;
        cmd.resp_type = MMC_RSP_R1b;
        davinci_mmc_send_cmd((void*)regs, &cmd, NULL);

        src += chunk * 512;
        sector += (int32_t)chunk;
        count -= chunk;
    }
    return 0;
}
