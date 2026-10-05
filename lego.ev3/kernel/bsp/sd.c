#include <stdio.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kmalloc.h>
#include <kstring.h>
#include <kernel/proc.h>
#include <kernel/hw_info.h>
#include <dev/sd.h>
#include <dev/timer.h>

#include "mmc.h"

/* u-boot davinci_mmc: 100000 polls x 10us = 1s per wait. The SD spec allows
 * up to 100ms read access time, so the old 10000 was only safe while the
 * delay below was a slow uncached spin loop. */
#define WATCHDOG_COUNT      (100000)
#define SD_RA_SECTORS       32
#define SD_MAX_BLOCKS       32

#define get_val(addr)       (*(volatile uint32_t*)(addr))
#define set_val(addr, val)  (*(volatile uint32_t*)(addr) = (val))
#define set_bit(addr, val)  set_val((addr), (get_val(addr) | (val)))
#define clear_bit(addr, val)    set_val((addr), (get_val(addr) & ~(val)))

/* Must be timer based: with I/D cache on, a CPU spin loop runs ~15x faster
 * than it did uncached, which silently shrank every SD timeout below the
 * card's worst-case latency and made the boot-time reads fail at random. */
static void delay_us(uint32_t us){
    _delay_usec(us);
}

/* Busy bit wait loop for MMCST1 */
static int dmmc_busy_wait(volatile struct davinci_mmc_regs *regs)
{
    uint32_t  wdog = WATCHDOG_COUNT;

    while (--wdog && (get_val(&regs->mmcst1) & MMCST1_BUSY))
        delay_us(10);

    if (wdog == 0)
        return -1;

    return 0;
}

/* Status bit wait loop for MMCST1 */
static int
dmmc_wait_fifo_status(volatile struct davinci_mmc_regs *regs, unsigned int status)
{
    int wdog = WATCHDOG_COUNT;

    while (--wdog && ((get_val(&regs->mmcst1) & status) != status))
        delay_us(10);

    if (!(get_val(&regs->mmcctl) & MMCCTL_WIDTH_4_BIT))
        delay_us(100);

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
        delay_us(10);

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
                    delay_us(600);
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

int32_t sd_init(void) {
    /* runs before timer_set_interval(); start TIM34 so delay_us() has a clock */
    timer_init();
    return 0;
}

static uint8_t _sector_buf[SD_RA_SECTORS * 512];
static int32_t _ra_start = -1;
static uint32_t _ra_count = 0;
static int32_t _pending_sector = -1;

static int32_t sd_read_multi(int32_t sector, uint32_t count, uint8_t* buf) {
    struct mmc_cmd cmd;
    struct mmc_data data;

    if(count == 1) {
        cmd.cmdidx = MMC_CMD_READ_SINGLE_BLOCK;
        cmd.cmdarg = sector;
        cmd.resp_type = MMC_RSP_R1;
        data.un.dest = (char*)buf;
        data.blocks = 1;
        data.blocksize = 512;
        data.flags = MMC_DATA_READ;
        if(davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, &data))
            return -1;
        return 0;
    }

    cmd.cmdidx = MMC_CMD_READ_MULTIPLE_BLOCK;
    cmd.cmdarg = sector;
    cmd.resp_type = MMC_RSP_R1;
    data.un.dest = (char*)buf;
    data.blocks = count;
    data.blocksize = 512;
    data.flags = MMC_DATA_READ;

    if(davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, &data)) {
        /* multi-block failed, fallback to single */
        cmd.cmdidx = MMC_CMD_READ_SINGLE_BLOCK;
        cmd.cmdarg = sector;
        cmd.resp_type = MMC_RSP_R1;
        data.blocks = 1;
        if(davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, &data))
            return -1;
        return 0;  /* only 1 sector valid */
    }

    /* CMD12 stop (R1, no busy for read-stop) */
    cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
    cmd.cmdarg = 0;
    cmd.resp_type = MMC_RSP_R1;
    davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, NULL);
    return 0;
}

int32_t sd_dev_read(int32_t sector) {
    if(sector < 0)
        return -1;

    /* Check readahead window hit */
    if(_ra_start >= 0 && sector >= _ra_start &&
       (uint32_t)(sector - _ra_start) < _ra_count) {
        _pending_sector = sector;
        return 0;
    }

    /* Fill readahead window: read up to SD_RA_SECTORS starting at sector */
    uint32_t count = SD_RA_SECTORS;
    if(sd_read_multi(sector, count, _sector_buf) != 0)
        return -1;

    _ra_start = sector;
    _ra_count = count;
    _pending_sector = sector;
    return 0;
}

int32_t sd_dev_read_done(void* buf) {
    if(_pending_sector < 0 || _ra_start < 0)
        return -1;

    uint32_t offset = (uint32_t)(_pending_sector - _ra_start) * 512;
    if(offset >= _ra_count * 512)
        return -1;

    memcpy(buf, _sector_buf + offset, 512);
    return 0;
}

int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
    uint8_t* out = (uint8_t*)buf;

    if(buf == 0 || count == 0)
        return -1;

    /* Invalidate readahead window */
    _ra_start = -1;
    _ra_count = 0;

    while(count > 0) {
        uint32_t chunk = (count > SD_MAX_BLOCKS) ? SD_MAX_BLOCKS : count;
        struct mmc_cmd cmd;
        struct mmc_data data;

        if(chunk == 1) {
            if(sd_read_multi(sector, 1, _sector_buf) != 0)
                return -1;
            memcpy(out, _sector_buf, 512);
            sector++;
            out += 512;
            count--;
            continue;
        }

        /* Try CMD18 multi-block */
        cmd.cmdidx = MMC_CMD_READ_MULTIPLE_BLOCK;
        cmd.cmdarg = sector;
        cmd.resp_type = MMC_RSP_R1;
        data.un.dest = (char*)_sector_buf;
        data.blocks = chunk;
        data.blocksize = 512;
        data.flags = MMC_DATA_READ;

        if(davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, &data) == 0) {
            /* Success: send CMD12 stop */
            cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
            cmd.cmdarg = 0;
            cmd.resp_type = MMC_RSP_R1;
            davinci_mmc_send_cmd((void*)MMC_BASE, &cmd, NULL);

            memcpy(out, _sector_buf, chunk * 512);
            sector += (int32_t)chunk;
            out += chunk * 512;
            count -= chunk;
        } else {
            /* Multi-block failed: single-block fallback */
            for(uint32_t i = 0; i < chunk; i++) {
                if(sd_read_multi(sector + (int32_t)i, 1,
                        _sector_buf) != 0)
                    return -1;
                memcpy(out + i * 512, _sector_buf, 512);
            }
            sector += (int32_t)chunk;
            out += chunk * 512;
            count -= chunk;
        }
    }
    return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
    (void)sector;
    (void)buf;
    return 0;
}

int32_t sd_dev_write_done(void) {

    return 0;
}
