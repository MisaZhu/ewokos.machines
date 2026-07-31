#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>

#include "dwmmc.h"

extern int mmc_read_blocks(struct dwmci_host *host, void *dst, uint32_t sector);
extern int mmc_read_sectors(struct dwmci_host *host, void *dst, uint32_t sector,
		uint32_t count);
extern int mmc_write_blocks(struct dwmci_host *host, const void *src, uint32_t sector);

static struct dwmci_host dwc_host = {
    .fifoth_val = 0,
    .fifo_mode = true,
};

#define RK3506_SD_RETRY_COUNT 5U
/*
 * Sectors per CMD18 (64KB). The dwmmc data path is FIFO/PIO with the
 * watermark drained inside dwmci_data_transfer, so there is no DMA
 * boundary to respect; larger bursts only amortize the fixed
 * CMD18+CMD12 transaction overhead. On a chunk error the size still
 * collapses to 1 and ramps back via rk3506_sd_note_success.
 */
#define RK3506_SD_MULTI_CHUNK_SECTORS 128U
#define RK3506_SD_RECOVER_SUCCESS_STREAK 32U
#define RK3506_SD_RETRY_DELAY_US 2000U

static uint32_t _fast_chunk_sectors = RK3506_SD_MULTI_CHUNK_SECTORS;
static uint32_t _active_chunk_sectors = RK3506_SD_MULTI_CHUNK_SECTORS;
static uint32_t _stable_successes = 0;
static uint32_t _fifo_depth = 0;

static void rk3506_sd_setup_host(void);
static void rk3506_sd_update_clock(void);

static void rk3506_sd_note_success(void) {
	if(_active_chunk_sectors == _fast_chunk_sectors) {
		_stable_successes = 0;
		return;
	}

	if(++_stable_successes >= RK3506_SD_RECOVER_SUCCESS_STREAK) {
		_stable_successes = 0;
		_active_chunk_sectors <<= 1;
		if(_active_chunk_sectors > _fast_chunk_sectors)
			_active_chunk_sectors = _fast_chunk_sectors;
	}
}

static void rk3506_sd_note_chunk_error(void) {
	_stable_successes = 0;
	_active_chunk_sectors = 1U;
}

static int rk3506_sd_wait_reset(struct dwmci_host *host, uint32_t value) {
	uint32_t timeout = 10000U;

	dwmci_writel(host, DWMCI_CTRL, value);
	while(timeout-- > 0U) {
		if((dwmci_readl(host, DWMCI_CTRL) & value) == 0U)
			return 0;
	}
	return -1;
}

static void rk3506_sd_recover(void) {
	(void)rk3506_sd_wait_reset(&dwc_host, DWMCI_RESET_ALL);
	rk3506_sd_setup_host();
	/*
	 * A controller reset stops/desyncs the CIU clock domain. Like Linux
	 * dw_mci_reset, an update-clocks command must follow the reset to
	 * reload CLKDIV/CLKENA into the CIU, otherwise every subsequent
	 * command times out and the whole driver looks wedged.
	 */
	rk3506_sd_update_clock();
	dwmci_writel(&dwc_host, DWMCI_RINTSTS, DWMCI_INTMSK_ALL);
}

static int rk3506_sd_read_sector_once(int32_t sector, void* buf) {
	return mmc_read_blocks(&dwc_host, buf, sector);
}

static int rk3506_sd_write_sector_once(int32_t sector, const void* buf) {
        return mmc_write_blocks(&dwc_host, buf, sector);
}

static int rk3506_sd_verify_sector(int32_t sector, const uint8_t* src) {
        uint8_t verify[512];
        int ret;
        uint32_t attempt;

        for(attempt = 0; attempt < RK3506_SD_RETRY_COUNT; attempt++) {
                ret = rk3506_sd_read_sector_once(sector, verify);
                if(ret == 0)
                        return memcmp(verify, src, sizeof(verify)) == 0 ? 0 : -EIO;
                rk3506_sd_note_chunk_error();
                rk3506_sd_recover();
                usleep(RK3506_SD_RETRY_DELAY_US);
        }

        return ret;
}

static void rk3506_sd_update_clock(void) {
	uint32_t timeout = 100000U;

	dwmci_writel(&dwc_host, DWMCI_CMD, DWMCI_CMD_PRV_DAT_WAIT |
			DWMCI_CMD_UPD_CLK | DWMCI_CMD_START);
	while(timeout-- > 0U) {
		if((dwmci_readl(&dwc_host, DWMCI_CMD) & DWMCI_CMD_START) == 0U)
			return;
	}
	klog("rk3506_sd: update clock timeout\n");
}

static void rk3506_sd_setup_host(void) {
	uint32_t fifo_size;

	/*
	 * Probe the FIFO depth only once from the power-on/u-boot FIFOTH
	 * value: after we program the register below, re-reading it during
	 * recover would halve the derived depth every time.
	 */
	if(_fifo_depth == 0) {
		fifo_size = dwmci_readl(&dwc_host, DWMCI_FIFOTH);
		_fifo_depth = ((fifo_size & RX_WMARK_MASK) >> RX_WMARK_SHIFT) + 1;
	}
	fifo_size = _fifo_depth;
	dwc_host.fifoth_val = MSIZE(0x2) | RX_WMARK(fifo_size / 2 - 1) |
		TX_WMARK(fifo_size / 2);
	/* actually program the watermarks so TXDR/RXDR match fifoth_val */
	dwmci_writel(&dwc_host, DWMCI_FIFOTH, dwc_host.fifoth_val);
	/* max out the data/busy timeout counter, writes hold busy for long */
	dwmci_writel(&dwc_host, DWMCI_TMOUT, 0xFFFFFFFFU);
}

/**
 * initialize EMMC to read SDHC card
 */
int32_t rk3506_sd_init(void) {
	_mmio_base = mmio_map();
	if(_mmio_base == 0)
		return -1;
	dwc_host.ioaddr = (void*)(_mmio_base + 0x480000);
	_fifo_depth = 0;
	rk3506_sd_setup_host();
	dwmci_writel(&dwc_host, DWMCI_RINTSTS, DWMCI_INTMSK_ALL);
	_fast_chunk_sectors = RK3506_SD_MULTI_CHUNK_SECTORS;
	_active_chunk_sectors = _fast_chunk_sectors;
	_stable_successes = 0;
	return 0;
}


int32_t rk3506_sd_read_sector(int32_t sector, void* buf) {
	int ret;
	uint32_t attempt;

	if(buf == NULL)
		return -1;

	for(attempt = 0; attempt < RK3506_SD_RETRY_COUNT; attempt++) {
		ret = rk3506_sd_read_sector_once(sector, buf);
		if(ret == 0) {
			rk3506_sd_note_success();
			return 0;
		}
		rk3506_sd_note_chunk_error();
		rk3506_sd_recover();
		usleep(RK3506_SD_RETRY_DELAY_US);
	}

	return ret;
}

int32_t rk3506_sd_read_blocks(int32_t sector, void* buf, uint32_t count) {
	uint8_t *dst = (uint8_t*)buf;

	if(buf == NULL)
		return -1;
	while(count > 0) {
		uint32_t chunk = (_active_chunk_sectors > count) ?
				count : _active_chunk_sectors;
		int ret;

		if(chunk > 1) {
			ret = mmc_read_sectors(&dwc_host, dst, sector, chunk);
			if(ret == 0) {
				rk3506_sd_note_success();
				dst += chunk * 512U;
				sector += chunk;
				count -= chunk;
				continue;
			}
			/*
			 * Multi-block failures on DWMMC are often controller state
			 * issues. Drop back to single-sector reads instead of
			 * retrying the same CMD18 sequence repeatedly.
			 */
			rk3506_sd_note_chunk_error();
			rk3506_sd_recover();
		}

		ret = rk3506_sd_read_sector(sector, dst);
		if(ret != 0)
			return ret;
		dst += 512U;
		sector++;
		count--;
	}

	return 0;
}

int32_t rk3506_sd_write_sector(int32_t sector, const void* buf) {
        int ret = -1;
        uint32_t attempt;

        if(buf == NULL)
                return -1;

        for(attempt = 0; attempt < RK3506_SD_RETRY_COUNT; attempt++) {
                ret = rk3506_sd_write_sector_once(sector, buf);
                if(ret == 0) {
                        ret = rk3506_sd_verify_sector(sector, (const uint8_t*)buf);
                        if(ret == 0) {
                                rk3506_sd_note_success();
                                return 0;
                        }
                        klog("rk3506_sd: write verify mismatch sec %d\n", sector);
                }
                rk3506_sd_note_chunk_error();
                rk3506_sd_recover();
                usleep(RK3506_SD_RETRY_DELAY_US);
        }

        return ret;
}
