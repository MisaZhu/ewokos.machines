/*
 * sdio.c - SDIO helper layer (CCCR/FBR access, function enable, block
 *          size, CIS parse), ported from the raspix wlan driver.
 */
#include <types.h>
#include <utils/log.h>

#include "sdio.h"
#include "fcie5.h"
#include "mmc.h"

static unsigned int MMC_BLOCK_SIZE[8] = {0};

static unsigned int sdio_max_blocks_per_cmd(int fn) {
    (void)fn;
    /* CMD53 limit; CIFD PIO copes fine with long bursts */
    return 511;
}

/* Split an arbitrarily sized data transfer into several
 * IO_RW_EXTENDED commands. */
static int sdio_io_rw_ext_helper(int fn, int write,
    unsigned addr, int incr_addr, uint8_t *buf, unsigned size) {
    unsigned remainder = size;
    unsigned max_blocks;
    int ret;

    /* Do the bulk of the transfer using block mode (if supported). */
    if (MMC_BLOCK_SIZE[fn] > 0 && size > MMC_BLOCK_SIZE[fn]) {
        max_blocks = sdio_max_blocks_per_cmd(fn);

        while (remainder >= MMC_BLOCK_SIZE[fn]) {
            unsigned blocks;

            blocks = remainder / MMC_BLOCK_SIZE[fn];
            if (blocks > max_blocks)
                blocks = max_blocks;
            size = blocks * MMC_BLOCK_SIZE[fn];

            ret = mmc_io_rw_extended(write,
                fn, addr, incr_addr, buf,
                blocks, MMC_BLOCK_SIZE[fn]);
            if (ret)
                return ret;

            remainder -= size;
            buf += size;
            if (incr_addr)
                addr += size;
        }
    }

    /* Write the remainder using byte mode. */
    while (remainder > 0) {
        size = remainder;
        if (MMC_BLOCK_SIZE[fn] > 0)
            size = min(remainder, MMC_BLOCK_SIZE[fn]);
        /* Indicate byte mode by setting "blocks" = 0 */
        ret = mmc_io_rw_extended(write, fn, addr,
             incr_addr, buf, 0, size);
        if (ret)
            return ret;

        remainder -= size;
        buf += size;
        if (incr_addr)
            addr += size;
    }
    return 0;
}

int sdio_memcpy_fromio(int func, void *dst,
    unsigned int addr, int count) {
    return sdio_io_rw_ext_helper(func, 0, addr, 1, dst, count);
}

int sdio_memcpy_toio(int func, unsigned int addr,
    void *src, int count) {
    return sdio_io_rw_ext_helper(func, 1, addr, 1, src, count);
}

uint8_t sdio_readb(int func, unsigned int addr, int *err_ret) {
    int ret;
    uint8_t val;

    ret = mmc_io_rw_direct(0, func, addr, 0, &val);
    if (err_ret)
        *err_ret = ret;
    if (ret)
        return 0xFF;

    return val;
}

void sdio_writeb(int func, uint8_t b, unsigned int addr, int *err_ret) {
    int ret;

    ret = mmc_io_rw_direct(1, func, addr, b, NULL);
    if (err_ret)
        *err_ret = ret;
}

int sdio_readsb(int func, void *dst, unsigned int addr,
    int count) {
    return sdio_io_rw_ext_helper(func, 0, addr, 0, dst, count);
}

int sdio_writesb(int func, unsigned int addr, void *src,
    int count) {
    return sdio_io_rw_ext_helper(func, 1, addr, 0, src, count);
}

uint16_t sdio_readw(int func, unsigned int addr, int *err_ret) {
    int ret;
    uint16_t val;

    ret = sdio_memcpy_fromio(func, &val, addr, 2);
    if (err_ret)
        *err_ret = ret;
    if (ret)
        return 0xFFFF;

    return val;
}

void sdio_writew(int func, uint16_t b, unsigned int addr, int *err_ret) {
    int ret;

    ret = sdio_memcpy_toio(func, addr, &b, 2);
    if (err_ret)
        *err_ret = ret;
}

uint32_t sdio_readl(int func, unsigned int addr, int *err_ret) {
    int ret;
    uint32_t val;

    ret = sdio_memcpy_fromio(func, &val, addr, 4);
    if (err_ret)
        *err_ret = ret;
    if (ret)
        return 0xFFFFFFFF;

    return val;
}

void sdio_writel(int func, uint32_t b, unsigned int addr, int *err_ret) {
    int ret;

    ret = sdio_memcpy_toio(func, addr, &b, 4);
    if (err_ret)
        *err_ret = ret;
}

int sdio_reset(void) {
    int ret;
    uint8_t abort;

    /* SDIO Simplified Specification V2.0, 4.4 Reset for SDIO */

    ret = mmc_io_rw_direct_host(0, 0, SDIO_CCCR_ABORT, 0, &abort);
    if (ret)
        abort = 0x08;
    else
        abort |= 0x08;

    return mmc_io_rw_direct_host(1, 0, SDIO_CCCR_ABORT, abort, NULL);
}

int sdio_set_block_size(unsigned int func, unsigned int blksz) {
    int ret;

    ret = mmc_io_rw_direct(1, 0, SDIO_FBR_BASE(func) + SDIO_FBR_BLKSIZE, blksz & 0xff, NULL);
    if (ret)
        return ret;

    ret = mmc_io_rw_direct(1, 0, SDIO_FBR_BASE(func) + SDIO_FBR_BLKSIZE + 1, (blksz >> 8) & 0xff, NULL);
    if (ret)
        return ret;

    if (func < sizeof(MMC_BLOCK_SIZE) / sizeof(unsigned int))
        MMC_BLOCK_SIZE[func] = blksz;

    return 0;
}

int sdio_enable_func(int func) {
    int ret;
    unsigned char reg;
    unsigned long timeout;

    ret = mmc_io_rw_direct(0, 0, SDIO_CCCR_IOEx, 0, &reg);
    if (ret)
        goto err;

    reg |= 1 << func;

    ret = mmc_io_rw_direct(1, 0, SDIO_CCCR_IOEx, reg, NULL);
    if (ret)
        goto err;

    timeout = get_timer(0) + 3000;

    while (1) {
        ret = mmc_io_rw_direct(0, 0, SDIO_CCCR_IORx, 0, &reg);
        if (ret)
            goto err;
        if (reg & (1 << func))
            break;
        ret = -ETIME;
        if (get_timer(timeout) > 0)
            goto err;
    }

    return 0;

err:
    wifi_log("SDIO: Failed to enable device %d\n", func);
    return ret;
}

int sdio_disable_func(int func) {
    int ret;
    unsigned char reg;

    if (!func)
        return -EINVAL;

    ret = mmc_io_rw_direct(0, 0, SDIO_CCCR_IOEx, 0, &reg);
    if (ret)
        goto err;

    reg &= ~(1 << func);

    ret = mmc_io_rw_direct(1, 0, SDIO_CCCR_IOEx, reg, NULL);
    if (ret)
        goto err;

    return 0;

err:
    wifi_log("SDIO: Failed to disable device %d\n", func);
    return ret;
}

int sdio_claim_irq(int func) {
    int ret;
    unsigned char reg;

    ret = mmc_io_rw_direct(0, 0, SDIO_CCCR_IENx, 0, &reg);
    if (ret)
        goto err;

    reg |= 1 << func;
    reg |= 1; /* Master interrupt enable */

    ret = mmc_io_rw_direct(1, 0, SDIO_CCCR_IENx, reg, NULL);
    if (ret)
        goto err;

    fcie5_enable_sdio_int(1);
    return 0;

err:
    wifi_log("SDIO: Failed to claim irq %d\n", func);
    return ret;
}

/*
 * Walk the common CIS and return the CISTPL_MANFID vendor/device
 * pair (used to verify we really talk to an RTL8723CS).
 */
int sdio_get_manfid(uint16_t *vendor, uint16_t *device) {
    uint32_t cis_ptr = 0;
    int ret, i;

    for (i = 0; i < 3; i++) {
        uint8_t x;
        ret = mmc_io_rw_direct(0, 0, SDIO_CCCR_CIS + i, 0, &x);
        if (ret)
            return ret;
        cis_ptr |= ((uint32_t)x) << (i * 8);
    }
    if (cis_ptr == 0 || cis_ptr >= 0x1F000)
        return -EINVAL;

    for (i = 0; i < 256; i++) {
        uint8_t tpl_code, tpl_link;

        ret = mmc_io_rw_direct(0, 0, cis_ptr++, 0, &tpl_code);
        if (ret)
            return ret;
        if (tpl_code == CISTPL_NULL)
            continue;
        if (tpl_code == CISTPL_END)
            break;

        ret = mmc_io_rw_direct(0, 0, cis_ptr++, 0, &tpl_link);
        if (ret)
            return ret;

        if (tpl_code == CISTPL_MANFID && tpl_link >= 4) {
            uint8_t b[4];
            int j;
            for (j = 0; j < 4; j++) {
                ret = mmc_io_rw_direct(0, 0, cis_ptr + j, 0, &b[j]);
                if (ret)
                    return ret;
            }
            *vendor = b[0] | (b[1] << 8);
            *device = b[2] | (b[3] << 8);
            return 0;
        }
        cis_ptr += tpl_link;
    }
    return -ENODEV;
}
