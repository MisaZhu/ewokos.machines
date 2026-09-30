#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <sysinfo.h>

#include "mmc.h"

#define WATCHDOG_COUNT      (1000000)
#define EV3_SD_MAX_BLOCKS   32

#define get_val(addr)       (*(volatile uint32_t*)(addr))
#define set_val(addr, val)  (*(volatile uint32_t*)(addr) = (val))
#define set_bit(addr, val)  set_val((addr), (get_val(addr) | (val)))
#define clear_bit(addr, val)    set_val((addr), (get_val(addr) & ~(val)))

/* Busy bit wait loop for MMCST1 */
static int dmmc_busy_wait(volatile struct davinci_mmc_regs *regs)
{
    uint32_t  wdog = WATCHDOG_COUNT;

    while (--wdog && (get_val(&regs->mmcst1) & MMCST1_BUSY));

    if (wdog == 0)
        return -1;

    return 0;
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
        /* clear previous data transfer if any and set new one */
        bytes_left = (data->blocksize * data->blocks);

        /* Reset FIFO - Always use 32 byte fifo threshold */
        set_val(&regs->mmcfifoctl,
                (MMCFIFOCTL_FIFOLEV | MMCFIFOCTL_FIFORST));

        cmddata |= MMCCMD_DMATRIG;

        cmddata |= MMCCMD_WDATX;
        if (data->flags == MMC_DATA_READ) {
            set_val(&regs->mmcfifoctl, MMCFIFOCTL_FIFOLEV);
        } else if (data->flags == MMC_DATA_WRITE) {
            set_val(&regs->mmcfifoctl,
                    (MMCFIFOCTL_FIFOLEV |
                     MMCFIFOCTL_FIFODIR));
            cmddata |= MMCCMD_DTRW;
        }

        set_val(&regs->mmctod, 0xFFFF);
        set_val(&regs->mmcnblk, (data->blocks & MMCNBLK_NBLK_MASK));
        set_val(&regs->mmcblen, (data->blocksize & MMCBLEN_BLEN_MASK));

        if (data->flags == MMC_DATA_WRITE) {
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
    if (err)
        return err;

    /* For R1b wait for busy done */
    if (cmd->resp_type == MMC_RSP_R1b)
        dmmc_busy_wait(regs);

    /* Collect response from controller for specific commands */
    if (mmcstatus & MMCST0_RSPDNE) {
        /* Copy the response to the response buffer */
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

    if (data->flags == MMC_DATA_READ) {
        /* check for DATDNE along with DRRDY as the controller might
         * set the DATDNE without DRRDY for smaller transfers with
         * less than FIFO threshold bytes
         */
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
            /*
             * MMC controller sets the Data receive ready bit
             * (DRRDY) in MMCST0 even before the entire FIFO is
             * full. This results in erratic behavior if we start
             * reading the FIFO soon after DRRDY.  Wait for the
             * FIFO full bit in MMCST1 for proper FIFO clearing.
             */
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
            /*
             * MMC controller sets the Data transmit ready bit
             * (DXRDY) in MMCST0 even before the entire FIFO is
             * empty. This results in erratic behavior if we start
             * writing the FIFO soon after DXRDY.  Wait for the
             * FIFO empty bit in MMCST1 for proper FIFO clearing.
             */
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

int32_t ev3_sd_init(void) {
    _mmio_base = mmio_map();

    /* Ensure 4-bit bus width is enabled (bootloader may have set it,
     * but re-apply in case of warm restart). ACMD6 = CMD55 + CMD6. */
    volatile struct davinci_mmc_regs *regs =
        (volatile struct davinci_mmc_regs*)(_mmio_base + MMC_BASE);

    struct mmc_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* CMD55 (APP_CMD) */
    cmd.cmdidx = MMC_CMD_APP_CMD;
    cmd.cmdarg = 0;
    cmd.resp_type = MMC_RSP_R1;
    davinci_mmc_send_cmd((void*)regs, &cmd, NULL);

    /* ACMD6: SET_BUS_WIDTH = 4-bit (arg=2) */
    cmd.cmdidx = SD_CMD_APP_SET_BUS_WIDTH;
    cmd.cmdarg = 2;
    cmd.resp_type = MMC_RSP_R1;
    if(davinci_mmc_send_cmd((void*)regs, &cmd, NULL) == 0) {
        set_bit(&regs->mmcctl, MMCCTL_WIDTH_4_BIT);
    }

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
