#include <dev/sd.h>

#include "sdmmc.h"
#include <mm/mmu.h>

uint8_t *_sector_buf = 0x87E00000;
static SDMMCBusWidthEmType _fast_bus_width = EV_BUS_4BITS;
static SDMMCBusWidthEmType _active_bus_width = EV_BUS_4BITS;
static uint32_t _stable_successes = 0;
// static IPEmType ge_IPSlot[3]     = {D_SDMMC1_IP, D_SDMMC2_IP, D_SDMMC3_IP};
// static PortEmType ge_PORTSlot[3] = {D_SDMMC1_PORT, D_SDMMC2_PORT, D_SDMMC3_PORT};
// static PADEmType  ge_PADSlot[3]  = {D_SDMMC1_PAD, D_SDMMC2_PAD, D_SDMMC3_PAD};
// static U32_T  gu32_MaxClkSlot[3] = {V_SDMMC1_MAX_CLK, V_SDMMC2_MAX_CLK, V_SDMMC3_MAX_CLK};

#define MIYOO_SD_REAL_CLK_HZ 8000000U
#define MIYOO_SD_RETRY_COUNT 5U
#define MIYOO_SD_RECOVER_SUCCESS_STREAK 32U

static void miyoo_sd_apply_bus_width(SDMMCBusWidthEmType bus_width) {
	_active_bus_width = bus_width;
	Hal_SDMMC_SetDataWidth(EV_IP_FCIE1, _active_bus_width);
	Hal_SDMMC_SetBusTiming(EV_IP_FCIE1, EV_BUS_DEF);
	Hal_SDMMC_SetNrcDelay(EV_IP_FCIE1, MIYOO_SD_REAL_CLK_HZ);
}

static void miyoo_sd_note_success(void) {
	if(_active_bus_width == _fast_bus_width) {
		_stable_successes = 0;
		return;
	}

	if(++_stable_successes >= MIYOO_SD_RECOVER_SUCCESS_STREAK) {
		_stable_successes = 0;
		miyoo_sd_apply_bus_width(_fast_bus_width);
	}
}

static void miyoo_sd_note_retryable_error(void) {
	_stable_successes = 0;
	if(_active_bus_width != EV_BUS_1BIT)
		miyoo_sd_apply_bus_width(EV_BUS_1BIT);
}

static void miyoo_sd_recover(void) {
	Hal_SDMMC_Reset(EV_IP_FCIE1);
	miyoo_sd_apply_bus_width(_active_bus_width);
}

static int miyoo_sd_should_retry(RspErrEmType err) {
	ErrGrpEmType group;

	if(err == EV_STS_OK)
		return 0;
	group = Hal_SDMMC_ErrGroup(err);
	return (group == EV_EGRP_TOUT) || (group == EV_EGRP_COMM);
}

uint16_t SDMMC_Init(uint8_t u8Slot)
{
	IPEmType eIP  = EV_IP_FCIE1;
	RspStruct * eRspSt;

	//_SDMMC_InfoInit(u8Slot);

	// SDMMC_SwitchPAD(u8Slot);
	// SDMMC_SetPower(u8Slot, EV_POWER_OFF);
	// SDMMC_SetPower(u8Slot, EV_POWER_ON);
	// SDMMC_SetPower(u8Slot, EV_POWER_UP);

	// SDMMC_SetClock(u8Slot, 400000, 0);
	// SDMMC_SetBusTiming(u8Slot, EV_BUS_LOW);

	// Hal_SDMMC_SetDataWidth(eIP, EV_BUS_1BIT);
	//Hal_SDMMC_SetSDIOClk(eIP, TRUE); //For Measure Clock, Don't Stop Clock

	_fast_bus_width = Hal_SDMMC_GetDataWidth(eIP);
	if(_fast_bus_width == EV_BUS_1BIT)
		_fast_bus_width = EV_BUS_4BITS;
	_stable_successes = 0;
	miyoo_sd_apply_bus_width(_fast_bus_width);

    // //--------------------------------------------------------------------------------------------------------
    // eRspSt = _SDMMC_Identification(u8Slot);
    // //--------------------------------------------------------------------------------------------------------
    // if(eRspSt->eErrCode)
    //     pr_sd_info(" errCmd:%d:x%x\n", eRspSt->u8Cmd, eRspSt->eErrCode);
    // if(eRspSt->eErrCode)
	// 	return (uint16_t)eRspSt->eErrCode;

    // //--------------------------------------------------------------------------------------------------------
    // eRspSt = _SDMMC_CMDReq(u8Slot, 9, 0, EV_R2);  //CMD9
    // //--------------------------------------------------------------------------------------------------------
    // if(eRspSt->eErrCode)
	// 	return (uint16_t)eRspSt->eErrCode;
	// else
	// 	_SDMMC_GetCSDInfo(u8Slot, eRspSt->u8ArrRspToken+1);

    // //--------------------------------------------------------------------------------------------------------
    // eRspSt = _SDMMC_SEND_STAUS(u8Slot); //CMD13
    // //--------------------------------------------------------------------------------------------------------
    // if(eRspSt->eErrCode)
	// 	return (uint16_t)eRspSt->eErrCode;

    // //--------------------------------------------------------------------------------------------------------
    // eRspSt = _SDMMC_CMDReq(u8Slot,  7, 0, EV_R1B); //CMD7;
    // //--------------------------------------------------------------------------------------------------------
    // if(eRspSt->eErrCode)
	// 	return (uint16_t)eRspSt->eErrCode;

	// eRspSt = _SDMMC_SEND_SCR(u8Slot, _sector_buf);


    // return (uint16_t)eRspSt->eErrCode;
	return 0;

}

int32_t sd_init(void) {
	return SDMMC_Init(0);
}

static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg, uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType, volatile uint8_t *pu8Buf)
{
	IPEmType eIP = EV_IP_FCIE1;
	//RspErrEmType eErr  = EV_STS_OK;
	CmdEmType eCmdType = EV_CMDREAD;
	RspStruct * eRspSt;
	bool bCloseClock = FALSE;

	//printf("_[sdmmc_%u] CMD_%u (0x%08X)__(TB: %u)(BSz: %u)", u8Slot, u8Cmd, u32Arg, u16BlkCnt, u16BlkSize);

	if( (u8Cmd == 24) || (u8Cmd==25))
		eCmdType = EV_CMDWRITE;

	Hal_SDMMC_SetCmdToken(eIP, u8Cmd, u32Arg);
	Hal_SDMMC_TransCmdSetting(eIP, eTransType, u16BlkCnt, u16BlkSize, Hal_CARD_TransMIUAddr(V2P(pu8Buf)), pu8Buf);
	Hal_SDMMC_SendCmdAndWaitProcess(eIP, eTransType, eCmdType, EV_R1, bCloseClock);
	eRspSt = Hal_SDMMC_GetRspToken(eIP);

	//printf("=> (Err: 0x%04X)\n", (uint16_t)eRspSt->eErrCode);
	return eRspSt;


}

int32_t sd_dev_read(int32_t sector) {
	RspStruct *rsp = 0;
	int retry;

	for(retry = 0; retry < MIYOO_SD_RETRY_COUNT; retry++) {
		rsp = _SDMMC_DATAReq(0, 17, sector, 1, 512, EV_DMA, _sector_buf);  //CMD17
		if(rsp->eErrCode == EV_STS_OK) {
			miyoo_sd_note_success();
			return 0;
		}
		if(!miyoo_sd_should_retry(rsp->eErrCode))
			return rsp->eErrCode;
		miyoo_sd_note_retryable_error();
		miyoo_sd_recover();
	}

	return rsp == 0 ? -1 : rsp->eErrCode;
}

int32_t sd_dev_read_done(void* buf) {
	memcpy(buf, _sector_buf, 512);
	return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
	return -1;
}

int32_t sd_dev_write_done(void) {
	return -1;
}
