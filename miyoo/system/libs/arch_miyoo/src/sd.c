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

#define MIYOO_SD_BOUNCE_SECTORS 128U
#define MIYOO_SD_BOUNCE_SIZE (MIYOO_SD_BOUNCE_SECTORS * 512U)
#define MIYOO_SD_BOUNCE_PHY 0x27e00000U
#define MIYOO_SD_BOUNCE_VIRT 0x87e00000U
#define MIYOO_SD_DMA_VIRT_OFFSET 0x60000000U
#define MIYOO_SD_REAL_CLK_HZ 8000000U
#define MIYOO_SD_ADMA_DESC_SIZE ((uint32_t)sizeof(AdmaDescStruct))
#define MIYOO_SD_ADMA_MAX_SECTORS (MIYOO_SD_BOUNCE_SECTORS - 1U)
#define MIYOO_SD_ADMA_DESC_OFFSET (MIYOO_SD_ADMA_MAX_SECTORS * 512U)

static inline uint32_t miyoo_sd_dma_addr(volatile uint8_t *buf) {
	ewokos_addr_t phy = (ewokos_addr_t)buf - (ewokos_addr_t)MIYOO_SD_DMA_VIRT_OFFSET;
	return Hal_CARD_TransMIUAddr((uint32_t)phy);
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
	SDMMCBusWidthEmType bus_width;

	_mmio_base = mmio_map();
	if(_mmio_base == NULL)
		return -1;
	_sector_buf = (uint8_t*)MIYOO_SD_BOUNCE_VIRT;
	_adma_desc = (AdmaDescStruct*)(MIYOO_SD_BOUNCE_VIRT + MIYOO_SD_ADMA_DESC_OFFSET);
	if(syscall3(SYS_MEM_MAP, (ewokos_addr_t)_sector_buf, MIYOO_SD_BOUNCE_PHY, MIYOO_SD_BOUNCE_SIZE) == 0)
		return -1;
	bus_width = Hal_SDMMC_GetDataWidth(EV_IP_FCIE1);
	sdmmc_init();
	Hal_SDMMC_SetDataWidth(EV_IP_FCIE1, bus_width);
	Hal_SDMMC_SetBusTiming(EV_IP_FCIE1, EV_BUS_DEF);
	Hal_SDMMC_SetNrcDelay(EV_IP_FCIE1, MIYOO_SD_REAL_CLK_HZ);
	return 0;
}


int32_t miyoo_sd_read_sector(int32_t sector, void* buf) {
	return miyoo_sd_read_blocks(sector, buf, 1);
}

int32_t miyoo_sd_write_sector(int32_t sector, const void* buf) {
	return miyoo_sd_write_blocks(sector, buf, 1);
}

int32_t miyoo_sd_read_blocks(int32_t sector, void* buf, uint32_t count) {
	static RspStruct * pstRsp;
	uint8_t* dst = (uint8_t*)buf;

	if(buf == NULL)
		return -1;

	while(count > 0) {
		uint32_t chunk = count;
		uint8_t cmd;

		if(chunk > MIYOO_SD_ADMA_MAX_SECTORS)
			chunk = MIYOO_SD_ADMA_MAX_SECTORS;

		cmd = (chunk > 1U) ? 18 : 17;
		pstRsp = _SDMMC_DATAReq(0, cmd, sector, (uint16_t)chunk, 512, EV_ADMA, _sector_buf);
		if(pstRsp->eErrCode != 0)
			return pstRsp->eErrCode;

		memcpy(dst, _sector_buf, chunk * 512U);
		dst += chunk * 512U;
		sector += (int32_t)chunk;
		count -= chunk;
	}
	return 0;
}

int32_t miyoo_sd_write_blocks(int32_t sector, const void* buf, uint32_t count) {
	static RspStruct * pstRsp;
	const uint8_t* src = (const uint8_t*)buf;

	if(buf == NULL)
		return -1;

	while(count > 0) {
		uint32_t chunk = count;
		uint8_t cmd;

		if(chunk > MIYOO_SD_BOUNCE_SECTORS)
			chunk = MIYOO_SD_BOUNCE_SECTORS;

		memcpy(_sector_buf, src, chunk * 512U);
		cmd = (chunk > 1) ? 25 : 24;
		pstRsp = _SDMMC_DATAReq(0, cmd, sector, (uint16_t)chunk, 512, EV_DMA, _sector_buf);
		if(pstRsp->eErrCode != 0)
			return pstRsp->eErrCode;

		src += chunk * 512U;
		sector += (int32_t)chunk;
		count -= chunk;
	}
	return 0;
}
