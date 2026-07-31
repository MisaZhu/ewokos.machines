#include <dev/sd.h>

#include "sdmmc.h"
#include <mm/mmu.h>

#define MIYOO_SD_BOUNCE_VIRT 0x87E00000U
#define MIYOO_SD_REAL_CLK_HZ 8000000U
#define MIYOO_SD_BOUNCE_SECTORS 128U
#define MIYOO_SD_READAHEAD_SMALL 4U
#define MIYOO_SD_READAHEAD_LARGE 32U
#define MIYOO_SD_RETRY_COUNT 5U
#define MIYOO_SD_RECOVER_SUCCESS_STREAK 32U

static uint8_t *_sector_buf = (uint8_t*)MIYOO_SD_BOUNCE_VIRT;
static SDMMCBusWidthEmType _fast_bus_width = EV_BUS_4BITS;
static SDMMCBusWidthEmType _active_bus_width = EV_BUS_4BITS;
static uint32_t _stable_successes = 0;
static int32_t _ra_start_sector = -1;
static uint32_t _ra_sector_count = 0;
static int32_t _pending_sector = -1;
static int32_t _last_done_sector = -1;

static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg,
                uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType,
                volatile uint8_t *pu8Buf);

static inline int miyoo_sd_ra_hit(int32_t sector) {
        return _ra_start_sector >= 0 &&
                sector >= _ra_start_sector &&
                (uint32_t)(sector - _ra_start_sector) < _ra_sector_count;
}

static inline void miyoo_sd_ra_invalidate(void) {
        _ra_start_sector = -1;
        _ra_sector_count = 0;
        _pending_sector = -1;
}

static inline uint32_t miyoo_sd_pick_ra_window(int32_t sector) {
        if(_last_done_sector >= 0 && sector == (_last_done_sector + 1))
                return MIYOO_SD_READAHEAD_LARGE;
        return MIYOO_SD_READAHEAD_SMALL;
}

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
        miyoo_sd_ra_invalidate();
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
        RspStruct *rsp = 0;
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
        }

        return rsp == 0 ? EV_OTHER_ERR : rsp->eErrCode;
}

uint16_t SDMMC_Init(uint8_t u8Slot)
{
        IPEmType eIP = EV_IP_FCIE1;

        (void)u8Slot;
        _fast_bus_width = Hal_SDMMC_GetDataWidth(eIP);
        if(_fast_bus_width == EV_BUS_1BIT)
                _fast_bus_width = EV_BUS_4BITS;

        _stable_successes = 0;
        miyoo_sd_ra_invalidate();
        miyoo_sd_apply_bus_width(_fast_bus_width);

        return 0;
}

int32_t sd_init(void) {
        return SDMMC_Init(0);
}

static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg,
                uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType,
                volatile uint8_t *pu8Buf)
{
        IPEmType eIP = EV_IP_FCIE1;
        CmdEmType eCmdType = EV_CMDREAD;
        RspStruct *eRspSt;
        bool bCloseClock = FALSE;

        (void)u8Slot;
        if((u8Cmd == 24) || (u8Cmd == 25))
                eCmdType = EV_CMDWRITE;

        Hal_SDMMC_SetCmdToken(eIP, u8Cmd, u32Arg);
        Hal_SDMMC_TransCmdSetting(eIP, eTransType, u16BlkCnt, u16BlkSize,
                        Hal_CARD_TransMIUAddr(V2P(pu8Buf)), pu8Buf);
        Hal_SDMMC_SendCmdAndWaitProcess(eIP, eTransType, eCmdType, EV_R1, bCloseClock);
        eRspSt = Hal_SDMMC_GetRspToken(eIP);
        return eRspSt;
}

static int32_t miyoo_sd_fill_ra_window(int32_t sector) {
        uint32_t i;
        uint32_t window = miyoo_sd_pick_ra_window(sector);

        /*
         * Kernel 早期启动优先稳定性，这里复用 system 的顺序窗口思路，
         * 但底层仍走已验证过的 CMD17 单块 DMA 路径，避免在早期环境
         * 引入 CMD18/ADMA/HS 组合带来的额外变量。
         */
        for(i = 0; i < window; i++) {
                RspErrEmType err = miyoo_sd_run_request(17, (uint32_t)(sector + (int32_t)i),
                                1, 512, EV_DMA, _sector_buf + i * 512U);
                if(err != EV_STS_OK) {
                        if(i == 0)
                                return err;
                        break;
                }
        }

        _ra_start_sector = sector;
        _ra_sector_count = i;
        return 0;
}

int32_t sd_dev_read(int32_t sector) {
        if(sector < 0)
                return -1;

        if(!miyoo_sd_ra_hit(sector)) {
                int32_t ret = miyoo_sd_fill_ra_window(sector);
                if(ret != 0)
                        return ret;
        }

        _pending_sector = sector;
        return 0;
}

int32_t sd_dev_read_done(void* buf) {
        uint32_t offset;

        if(buf == 0 || !miyoo_sd_ra_hit(_pending_sector))
                return -1;

        offset = (uint32_t)(_pending_sector - _ra_start_sector) * 512U;
        memcpy(buf, _sector_buf + offset, 512U);
        _last_done_sector = _pending_sector;
        return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
        (void)sector;
        (void)buf;
        return -1;
}

int32_t sd_dev_write_done(void) {
        return -1;
}
