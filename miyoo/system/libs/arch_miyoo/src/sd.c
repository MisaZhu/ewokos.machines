#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>

#include <arch/miyoo/sd.h>
#include "sdmmc.h"
static uint8_t *_sector_buf;
static AdmaDescStruct *_adma_desc;
static SDMMCBusWidthEmType _fast_bus_width = EV_BUS_4BITS;
static SDMMCBusWidthEmType _active_bus_width = EV_BUS_4BITS;
static uint32_t _stable_successes = 0;
static uint32_t _fast_chunk_sectors = 0;
static uint32_t _active_chunk_sectors = 0;

#define MIYOO_SD_BOUNCE_SECTORS 128U
#define MIYOO_SD_BOUNCE_SIZE (MIYOO_SD_BOUNCE_SECTORS * 512U)
#define MIYOO_SD_BOUNCE_PHY 0x27e00000U
#define MIYOO_SD_BOUNCE_VIRT 0x87e00000U
#define MIYOO_SD_DMA_VIRT_OFFSET 0x60000000U
#define MIYOO_SD_REAL_CLK_HZ 8000000U
#define MIYOO_SD_ADMA_DESC_SIZE ((uint32_t)sizeof(AdmaDescStruct))
#define MIYOO_SD_ADMA_MAX_SECTORS (MIYOO_SD_BOUNCE_SECTORS - 1U)
#define MIYOO_SD_ADMA_DESC_OFFSET (MIYOO_SD_ADMA_MAX_SECTORS * 512U)
#define MIYOO_SD_RETRY_COUNT 5U
#define MIYOO_SD_RETRY_DELAY_US 2000U
#define MIYOO_SD_RECOVER_SUCCESS_STREAK 32U
#define MIYOO_SD_SYSTEM_SAFE_CHUNK 1U
#define MIYOO_SD_SYSTEM_FAST_CHUNK  8U


static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg,
		uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType,
		volatile uint8_t *pu8Buf);

static inline uint32_t miyoo_sd_dma_addr(volatile uint8_t *buf) {
	ewokos_addr_t phy = (ewokos_addr_t)buf - (ewokos_addr_t)MIYOO_SD_DMA_VIRT_OFFSET;
	return Hal_CARD_TransMIUAddr((uint32_t)phy);
}

static void miyoo_sd_apply_bus_width(SDMMCBusWidthEmType bus_width) {
	_active_bus_width = bus_width;
	Hal_SDMMC_SetDataWidth(EV_IP_FCIE1, _active_bus_width);
	Hal_SDMMC_SetBusTiming(EV_IP_FCIE1, EV_BUS_DEF);
	Hal_SDMMC_SetNrcDelay(EV_IP_FCIE1, MIYOO_SD_REAL_CLK_HZ);
}

static void miyoo_sd_note_success(void) {
	if(_active_bus_width == _fast_bus_width &&
			_active_chunk_sectors == _fast_chunk_sectors) {
		_stable_successes = 0;
		return;
	}

	if(++_stable_successes >= MIYOO_SD_RECOVER_SUCCESS_STREAK) {
		_stable_successes = 0;
		if(_active_chunk_sectors < _fast_chunk_sectors) {
			_active_chunk_sectors <<= 1;
			if(_active_chunk_sectors > _fast_chunk_sectors)
				_active_chunk_sectors = _fast_chunk_sectors;
		}
		else {
			miyoo_sd_apply_bus_width(_fast_bus_width);
		}
	}
}

static void miyoo_sd_note_retryable_error(void) {
	_stable_successes = 0;
	_active_chunk_sectors = MIYOO_SD_SYSTEM_SAFE_CHUNK;
	if(_active_bus_width != EV_BUS_1BIT)
		miyoo_sd_apply_bus_width(EV_BUS_1BIT);
}

static void miyoo_sd_recover(void) {
	Hal_SDMMC_Reset(EV_IP_FCIE1);
	sdmmc_init();
	miyoo_sd_apply_bus_width(_active_bus_width);
}

static int miyoo_sd_should_retry(RspErrEmType err) {
	ErrGrpEmType group;

	if(err == EV_STS_OK)
		return 0;
	group = Hal_SDMMC_ErrGroup(err);
	return (group == EV_EGRP_TOUT) || (group == EV_EGRP_COMM);
}

static RspErrEmType miyoo_sd_run_request(uint8_t cmd, uint32_t sector,
		uint16_t blk_cnt, uint16_t blk_size, TransEmType trans_type,
		volatile uint8_t *buf) {
	RspStruct *rsp;
	uint32_t attempt;

	for(attempt = 0; attempt < MIYOO_SD_RETRY_COUNT; attempt++) {
		rsp = _SDMMC_DATAReq(0, cmd, sector, blk_cnt, blk_size, trans_type, buf);
		if(rsp->eErrCode == EV_STS_OK) {
			miyoo_sd_note_success();
			return EV_STS_OK;
		}
		if(!miyoo_sd_should_retry(rsp->eErrCode))
			return rsp->eErrCode;
		miyoo_sd_note_retryable_error();
		miyoo_sd_recover();
		usleep(MIYOO_SD_RETRY_DELAY_US);
	}

	return rsp->eErrCode;
}

static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg, uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType, volatile uint8_t *pu8Buf)
{
	IPEmType eIP = EV_IP_FCIE1;
	//RspErrEmType eErr  = EV_STS_OK;
	CmdEmType eCmdType = EV_CMDREAD;
	RspStruct * eRspSt;

	bool bCloseClock = FALSE;
	volatile uint8_t *cmd_buf = pu8Buf;
	uint32_t dma_addr = miyoo_sd_dma_addr(pu8Buf);

	//klog("_[sdmmc_%u] CMD_%u (0x%08X)__(TB: %u)(BSz: %u)", u8Slot, u8Cmd, u32Arg, u16BlkCnt, u16BlkSize);

	if( (u8Cmd == 24) || (u8Cmd==25))
		eCmdType = EV_CMDWRITE;

	/*
	 * Keep the SD clock running across data commands. The upper layers
	 * mostly issue many small adjacent sector reads during boot, and
	 * closing the clock on every single-sector DMA transfer amplifies
	 * latency without changing transfer semantics.
	 */
	bCloseClock = FALSE;

	if(eTransType == EV_ADMA) {
		Hal_SDMMC_ADMASetting(eIP, _adma_desc, 0,
			(uint32_t)u16BlkCnt * u16BlkSize,
			dma_addr,
			0,
			TRUE);
		cmd_buf = (volatile uint8_t*)_adma_desc;
		dma_addr = miyoo_sd_dma_addr(cmd_buf);
	}

	Hal_SDMMC_SetCmdToken(eIP, u8Cmd, u32Arg);
	Hal_SDMMC_TransCmdSetting(eIP, eTransType, u16BlkCnt, u16BlkSize, dma_addr, cmd_buf);
	Hal_SDMMC_SendCmdAndWaitProcess(eIP, eTransType, eCmdType, EV_R1, bCloseClock);
	eRspSt = Hal_SDMMC_GetRspToken(eIP);

	//klog("=> (Err: 0x%04X)\n", (uint16_t)eRspSt->eErrCode);
	return eRspSt;


}

/**
 * initialize EMMC to read SDHC card
 */
int32_t miyoo_sd_init(void) {
	SDMMCBusWidthEmType boot_bus_width;

	_mmio_base = mmio_map();
	if(_mmio_base == 0)
		return -1;
	_sector_buf = (uint8_t*)MIYOO_SD_BOUNCE_VIRT;
	_adma_desc = (AdmaDescStruct*)(MIYOO_SD_BOUNCE_VIRT + MIYOO_SD_ADMA_DESC_OFFSET);
	if(syscall3(SYS_MEM_MAP, (ewokos_addr_t)_sector_buf, MIYOO_SD_BOUNCE_PHY, MIYOO_SD_BOUNCE_SIZE) == 0)
		return -1;

	/*
	 * Preserve whatever bus width the bootloader/kernel left the FCIE5 in.
	 * Forcing 1BIT here flips the host from 4BIT to 1BIT without telling
	 * the card (no ACMD6), and the resulting timing mismatch corrupts
	 * multi-block transfers on cards that were negotiated to 4BIT.
	 */
	boot_bus_width = Hal_SDMMC_GetDataWidth(EV_IP_FCIE1);
	sdmmc_init();
	_fast_bus_width = boot_bus_width;
	_fast_chunk_sectors = MIYOO_SD_SYSTEM_FAST_CHUNK;
	_active_chunk_sectors = _fast_chunk_sectors;
	_stable_successes = 0;
	miyoo_sd_apply_bus_width(_fast_bus_width);

	/*
	 * These two set gu16_DDR_MODE_REG (pad/drive strength) and
	 * gu16_WT_NRC (command/response window); they MUST be re-issued
	 * because sdmmc_init() just zeroed them, and the multi-block path
	 * reads them on every command.
	 */
	Hal_SDMMC_SetBusTiming(EV_IP_FCIE1, EV_BUS_DEF);
	Hal_SDMMC_SetNrcDelay(EV_IP_FCIE1, MIYOO_SD_REAL_CLK_HZ);
	return 0;
}


int32_t miyoo_sd_read_sector(int32_t sector, void* buf) {
	RspErrEmType err;

	if(buf == NULL)
		return -1;
	/*
	 * Keep single-sector reads on the kernel-tested EV_DMA path; only the
	 * multi-sector path in miyoo_sd_try_read_multi() needs EV_ADMA, and
	 * mixing the two paths in the same function risks regressing what the
	 * kernel has been using to load the system image in the first place.
	 */
	err = miyoo_sd_run_request(17, sector, 1, 512, EV_DMA, _sector_buf);
	if(err != EV_STS_OK)
		return err;
	memcpy(buf, _sector_buf, 512U);
	return 0;
}

int32_t miyoo_sd_write_sector(int32_t sector, const void* buf) {
	RspErrEmType err;

	if(buf == NULL)
		return -1;
	memcpy(_sector_buf, buf, 512U);
	err = miyoo_sd_run_request(24, sector, 1, 512, EV_DMA, _sector_buf);
	if(err != EV_STS_OK)
		return err;
	return 0;
}

static RspErrEmType miyoo_sd_cmd12(IPEmType eIP) {
	bool bCloseClock = FALSE;
	/*
	 * CMD12 is a command-only (R1b) transfer: no data phase, but DAT0 stays
	 * busy until the card finishes its internal state transition, so we must
	 * use the R1b response path which waits for DAT0 high.
	 */
	Hal_SDMMC_SetCmdToken(eIP, 12, 0);
	Hal_SDMMC_TransCmdSetting(eIP, EV_EMP, 0, 0, 0, NULL);
	return Hal_SDMMC_SendCmdAndWaitProcess(eIP, EV_EMP, EV_CMDRSP, EV_R1B, bCloseClock);
}

static RspErrEmType miyoo_sd_try_read_multi(uint32_t sector, uint32_t count, volatile uint8_t* buf) {
	RspStruct *rsp;
	RspErrEmType err;
	/*
	 * FCIE5 多块读必须走 ADMA：DMA 模式下 JOB_BLK_CNT 直接等于块数，
	 * 但 Hal_SDMMC_SendCmdAndWaitProcess 的 R_DATA_END 是按 JOB_BLK_CNT
	 * 触发的，控制器硬件不会按 CMD18 的块数自动拆 8 个 block；
	 * ADMA descriptor 里的 u32_JobCnt=chunks 才会驱动多块续传。
	 *
	 * 重试在多块读这里是反效果：rdata 状态下重复发同一条 CMD18 必然超时，
	 * miyoo_sd_run_request 的 5×2s 耗时会直接打穿上层文件系统的读超时。
	 * 失败就让外层 miyoo_sd_read_blocks 走单块保底路径。
	 */
	if(count == 1)
		return miyoo_sd_run_request(17, sector, 1, 512, EV_ADMA, buf);

	rsp = _SDMMC_DATAReq(0, 18, sector, (uint16_t)count, 512, EV_ADMA, buf);
	err = rsp->eErrCode;

	/*
	 * CMD18 leaves the card in rdata state with DAT0 busy; the FCIE5 HAL
	 * does not auto-issue CMD12, so we must stop the transfer explicitly
	 * before the next command (or the very next read will time out).
	 * 失败也照发：把卡从 rdata 拉回 tran，否则后面的单块保底也会卡死。
	 */
	(void)miyoo_sd_cmd12(EV_IP_FCIE1);

	if(err == EV_STS_OK)
		miyoo_sd_note_success();
	return err;
}

static RspErrEmType miyoo_sd_try_write_multi(uint32_t sector, uint32_t count, const volatile uint8_t* buf) {
	RspErrEmType err;
	if(count == 1)
		return miyoo_sd_run_request(24, sector, 1, 512, EV_DMA, (volatile uint8_t*)buf);
	err = miyoo_sd_run_request(25, sector, count, 512, EV_DMA, (volatile uint8_t*)buf);
	/* Same reasoning as the read path: release the card from rdata. */
	(void)miyoo_sd_cmd12(EV_IP_FCIE1);
	return err;
}

int32_t miyoo_sd_read_blocks(int32_t sector, void* buf, uint32_t count) {
	uint8_t* dst = (uint8_t*)buf;

	if(buf == NULL)
		return -1;

	while(count > 0) {
		uint32_t chunk = (_active_chunk_sectors > count) ? count : _active_chunk_sectors;
		RspErrEmType err;

		if(chunk > 1 && (chunk * 512U) <= MIYOO_SD_BOUNCE_SIZE) {
			err = miyoo_sd_try_read_multi(sector, chunk, _sector_buf);
			if(err == EV_STS_OK) {
				memcpy(dst, _sector_buf, chunk * 512U);
				dst += chunk * 512U;
				sector += chunk;
				count -= chunk;
				continue;
			}
			/* 多块失败，退单块重试 */
			miyoo_sd_note_retryable_error();
			miyoo_sd_recover();
		}

		/* 单块保底 */
		err = miyoo_sd_run_request(17, sector, 1, 512, EV_DMA, _sector_buf);
		if(err != EV_STS_OK)
			return err;
		memcpy(dst, _sector_buf, 512U);
		dst += 512U;
		sector++;
		count--;
	}
	return 0;
}

int32_t miyoo_sd_write_blocks(int32_t sector, const void* buf, uint32_t count) {
	const uint8_t* src = (const uint8_t*)buf;

	if(buf == NULL)
		return -1;

	while(count > 0) {
		uint32_t chunk = (_active_chunk_sectors > count) ? count : _active_chunk_sectors;
		RspErrEmType err;

		if(chunk > 1 && (chunk * 512U) <= MIYOO_SD_BOUNCE_SIZE) {
			memcpy(_sector_buf, src, chunk * 512U);
			err = miyoo_sd_try_write_multi(sector, chunk, _sector_buf);
			if(err == EV_STS_OK) {
				src += chunk * 512U;
				sector += chunk;
				count -= chunk;
				continue;
			}
			/* 多块失败，退单块重试 */
			miyoo_sd_note_retryable_error();
			miyoo_sd_recover();
		}

		/* 单块保底 */
		memcpy(_sector_buf, src, 512U);
		err = miyoo_sd_run_request(24, sector, 1, 512, EV_DMA, _sector_buf);
		if(err != EV_STS_OK)
			return err;
		src += 512U;
		sector++;
		count--;
	}
	return 0;
}
