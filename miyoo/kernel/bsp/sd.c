/*
 * miyoo kernel-side SD driver.
 *
 * Rewritten to follow the system-side read flow:
 *   1) Full SD card initialization (no longer assumes an earlier stage left
 *      the card configured):
 *        CMD0 -> CMD8 -> ACMD41(poll OCR) -> CMD2 -> CMD3 -> CMD7(R1B busy)
 *        -> CMD13 poll until TRAN -> ACMD6(4-bit) -> CMD16(512)
 *      Every step checks its response, and a final CMD13 confirms the card
 *      is in TRAN with READY_FOR_DATA, so the card state is correct before
 *      entering the read path. A data read error reruns this whole
 *      initialization to recover.
 *   2) CMD6 (SWITCH, arg 0x80FFFFF1) to High-Speed:
 *        the 64-byte switch status is read back by DMA and the group1
 *        support and switch-result bits are checked; on success the host
 *        switches to EV_BUS_HS sampling timing, otherwise (failure or no
 *        support) the default speed is kept.
 *      Note: the host SD clock stays at the boot-stage 8MHz for now (the
 *      system clock-setting registers will be added separately once
 *      recovered from the firmware); this only does the card-side switch
 *      plus HS timing.
 *
 * Read path: real CMD18 multi-block reads + single-segment DMA (replacing the
 * per-block CMD17 loop).
 *   - the read-ahead window (4 -> 128 sectors) fills the bounce buffer with
 *     one CMD18;
 *   - after every read, success or not, CMD12 stops the transfer (R1B waits
 *     on DAT0), then CMD13 confirms the return to TRAN;
 *   - sd_dev_read_blocks reads bounce-sized chunks with CMD18 and copies
 *     them out.
 */
#include <dev/sd.h>

#include "sdmmc.h"
#include <mm/mmu.h>

#define MIYOO_SD_BOUNCE_VIRT 0x87E00000U
#define MIYOO_SD_REAL_CLK_HZ 8000000U
#define MIYOO_SD_BOUNCE_SECTORS 128U
#define MIYOO_SD_READAHEAD_SMALL 4U
#define MIYOO_SD_READAHEAD_LARGE 128U
#define MIYOO_SD_RETRY_COUNT 5U

#define MIYOO_SD_IP EV_IP_FCIE1

/* ---- SD command indexes ---- */
#define SD_CMD_GO_IDLE_STATE       0
#define SD_CMD_ALL_SEND_CID        2
#define SD_CMD_SEND_RELATIVE_ADDR  3
#define SD_CMD_SWITCH_FUNC         6
#define SD_CMD_SELECT_CARD         7
#define SD_CMD_SEND_IF_COND        8
#define SD_CMD_STOP_TRANSMISSION   12
#define SD_CMD_SEND_STATUS         13
#define SD_CMD_SET_BLOCKLEN        16
#define SD_CMD_READ_MULTIPLE_BLOCK 18
#define SD_CMD_APP_CMD             55
#define SD_ACMD_SD_SEND_OP_COND    41
#define SD_ACMD_SET_BUS_WIDTH      6

/* ---- OCR / R1 helpers ---- */
#define SD_OCR_BUSY       0x80000000U
#define SD_OCR_CCS        0x40000000U   /* card capacity status: 1 = SDHC/SDXC */
#define SD_OCR_VDD_27_36  0x00FF8000U
#define SD_ACMD41_ARG_V2  (SD_OCR_CCS | SD_OCR_VDD_27_36) /* HCS + 2.7~3.6V */
#define SD_ACMD41_ARG_V1  SD_OCR_VDD_27_36

#define SD_R1_CURRENT_STATE(st)  (((st) >> 9) & 0xFU)
#define SD_R1_READY_FOR_DATA(st) (((st) >> 8) & 0x1U)
#define SD_STATE_TRAN 4U

/* mode=switch(1), group1(access mode)=HS(1), other groups unchanged(0xF) */
#define SD_CMD6_ARG_HS      0x80FFFFF1U
#define SD_SWITCH_STS_BYTES 64U

#define SD_INIT_ACMD41_TIMEOUT_MS 2000U
#define SD_INIT_STATE_TIMEOUT_MS  500U
#define SD_STOP_STATE_TIMEOUT_MS  100U

static uint8_t *_sector_buf = (uint8_t*)MIYOO_SD_BOUNCE_VIRT;

typedef struct {
    int inited;    /* the full initialization sequence completed */
    int is_v2;     /* CMD8 answered: SD spec v2+ card */
    int is_sdhc;   /* OCR CCS: SDHC/SDXC, block addressing */
    int is_hs;     /* the card accepted the CMD6 high-speed switch */
    int bus_4bit;  /* the card accepted ACMD6 4-bit */
    uint16_t rca;
} MiyooSDCard;

static MiyooSDCard _card;

static int32_t _ra_start_sector = -1;
static uint32_t _ra_sector_count = 0;
static int32_t _pending_sector = -1;
static int32_t _last_done_sector = -1;

static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg,
                uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType,
                volatile uint8_t *pu8Buf);

static inline void sd_msleep(uint32_t ms) {
    _delay(ms * 1000U);
}

/* 32-bit payload of R1/R3/R6/R7: token[1..4], MSB first */
static inline uint32_t sd_rsp32(const RspStruct *rsp) {
    return ((uint32_t)rsp->u8ArrRspToken[1] << 24) |
           ((uint32_t)rsp->u8ArrRspToken[2] << 16) |
           ((uint32_t)rsp->u8ArrRspToken[3] << 8)  |
           (uint32_t)rsp->u8ArrRspToken[4];
}

/* ------------------------------------------------------------------
 * Read-ahead window management
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * Command send/receive wrappers
 * ------------------------------------------------------------------ */
static RspErrEmType miyoo_sd_cmd(uint8_t cmd, uint32_t arg, SDMMCRspEmType rsp_type) {
    Hal_SDMMC_SetCmdToken(MIYOO_SD_IP, cmd, arg);
    return Hal_SDMMC_SendCmdAndWaitProcess(MIYOO_SD_IP, EV_EMP, EV_CMDRSP,
                    rsp_type, FALSE);
}

static RspErrEmType miyoo_sd_acmd(uint16_t rca, uint8_t acmd, uint32_t arg,
                SDMMCRspEmType rsp_type) {
    RspErrEmType err = miyoo_sd_cmd(SD_CMD_APP_CMD, (uint32_t)rca << 16, EV_R1);
    if(err != EV_STS_OK)
        return err;
    return miyoo_sd_cmd(acmd, arg, rsp_type);
}

/* ------------------------------------------------------------------
 * Initialization sub-steps
 * ------------------------------------------------------------------ */

/* CMD8 probes for SD v2; no response or a CRC error continues as a legacy
 * v1 card */
static int miyoo_sd_probe_v2(void) {
    RspErrEmType err = miyoo_sd_cmd(SD_CMD_SEND_IF_COND, 0x1AAU, EV_R7);
    RspStruct *rsp = Hal_SDMMC_GetRspToken(MIYOO_SD_IP);

    if(err != EV_STS_OK)
        return 0;
    return (sd_rsp32(rsp) & 0xFFFU) == 0x1AAU;
}

/* Poll ACMD41 until the OCR busy bit is set (card power-up done) */
static int miyoo_sd_init_ocr(uint32_t arg) {
    uint32_t elapsed = 0;

    while(elapsed < SD_INIT_ACMD41_TIMEOUT_MS) {
        RspErrEmType err = miyoo_sd_acmd(0, SD_ACMD_SD_SEND_OP_COND, arg, EV_R3);
        RspStruct *rsp = Hal_SDMMC_GetRspToken(MIYOO_SD_IP);
        uint32_t ocr = sd_rsp32(rsp);

        if(err == EV_STS_OK && (ocr & SD_OCR_BUSY)) {
            if(!(ocr & SD_OCR_VDD_27_36))
                return -1; /* voltage range mismatch */
            _card.is_sdhc = (ocr & SD_OCR_CCS) ? 1 : 0;
            return 0;
        }
        sd_msleep(1);
        elapsed++;
    }
    return -1;
}

/* Poll CMD13 until CURRENT_STATE == the wanted state and READY_FOR_DATA */
static int miyoo_sd_wait_state(uint32_t want_state, uint32_t timeout_ms) {
    uint32_t elapsed = 0;

    while(elapsed <= timeout_ms) {
        RspErrEmType err = miyoo_sd_cmd(SD_CMD_SEND_STATUS,
                        (uint32_t)_card.rca << 16, EV_R1);
        RspStruct *rsp = Hal_SDMMC_GetRspToken(MIYOO_SD_IP);
        uint32_t st = sd_rsp32(rsp);

        if(err == EV_STS_OK &&
           SD_R1_CURRENT_STATE(st) == want_state &&
           SD_R1_READY_FOR_DATA(st))
            return 0;
        sd_msleep(1);
        elapsed++;
    }
    return -1;
}

/*
 * CMD6 switch to High-Speed. The 64-byte switch status is read by DMA into
 * the head of the bounce buffer (dev-mapped region, DMA-coherent, no cache
 * issue).
 * Checks (big-endian byte order):
 *   byte[13] bit1  -> group1 function1 (HS) support bit
 *   byte[16] 3:0   -> group1 actual switch result, must be 1 to succeed
 */
static int miyoo_sd_try_switch_hs(void) {
    volatile uint8_t *sts = _sector_buf;
    RspErrEmType err;
    uint32_t i;

    for(i = 0; i < SD_SWITCH_STS_BYTES; i++)
        sts[i] = 0;

    Hal_SDMMC_SetCmdToken(MIYOO_SD_IP, SD_CMD_SWITCH_FUNC, SD_CMD6_ARG_HS);
    Hal_SDMMC_TransCmdSetting(MIYOO_SD_IP, EV_DMA, 1, SD_SWITCH_STS_BYTES,
                    Hal_CARD_TransMIUAddr(V2P(sts)), sts);
    err = Hal_SDMMC_SendCmdAndWaitProcess(MIYOO_SD_IP, EV_DMA, EV_CMDREAD,
                    EV_R1, FALSE);
    if(err != EV_STS_OK)
        return -1;

    if(!(sts[13] & 0x02))
        return -1; /* card does not support HS */
    if((sts[16] & 0x0F) != 0x01)
        return -1; /* switch did not take effect */
    return 0;
}

/* ------------------------------------------------------------------
 * Full card initialization (shared by init and runtime error recovery)
 * ------------------------------------------------------------------ */
static int miyoo_sd_card_init(void) {
    IPEmType ip = MIYOO_SD_IP;
    RspErrEmType err;
    RspStruct *rsp;
    uint32_t retry;

    _card.inited = 0;
    _card.is_v2 = 0;
    _card.is_sdhc = 0;
    _card.is_hs = 0;
    _card.bus_4bit = 0;
    _card.rca = 0;
    miyoo_sd_ra_invalidate();

    Hal_SDMMC_Reset(ip);
    Hal_SDMMC_SetDataWidth(ip, EV_BUS_1BIT);
    Hal_SDMMC_SetBusTiming(ip, EV_BUS_DEF);
    Hal_SDMMC_SetNrcDelay(ip, MIYOO_SD_REAL_CLK_HZ);

    /* The clock stays on from the boot stage; add idle clocks here to meet
     * the >=74 clocks requirement */
    Hal_SDMMC_ClkCtrl(ip, TRUE, 1);

    /* CMD0: back to idle (a few retries allowed, covering warm reboots) */
    err = EV_OTHER_ERR;
    for(retry = 0; retry < 3; retry++) {
        err = miyoo_sd_cmd(SD_CMD_GO_IDLE_STATE, 0, EV_NO);
        if(err == EV_STS_OK)
            break;
        sd_msleep(1);
    }
    if(err != EV_STS_OK) {
        printf("[SD] CMD0 fail: 0x%X\n", err);
        return -1;
    }

    _card.is_v2 = miyoo_sd_probe_v2();

    if(miyoo_sd_init_ocr(_card.is_v2 ? SD_ACMD41_ARG_V2 : SD_ACMD41_ARG_V1) != 0) {
        printf("[SD] ACMD41 timeout/fail\n");
        return -1;
    }

    if(miyoo_sd_cmd(SD_CMD_ALL_SEND_CID, 0, EV_R2) != EV_STS_OK) {
        printf("[SD] CMD2 fail\n");
        return -1;
    }

    err = miyoo_sd_cmd(SD_CMD_SEND_RELATIVE_ADDR, 0, EV_R6);
    if(err != EV_STS_OK) {
        printf("[SD] CMD3 fail: 0x%X\n", err);
        return -1;
    }
    rsp = Hal_SDMMC_GetRspToken(ip);
    _card.rca = (uint16_t)(((uint16_t)rsp->u8ArrRspToken[1] << 8) |
                    rsp->u8ArrRspToken[2]);
    if(_card.rca == 0) {
        printf("[SD] CMD3 bad RCA\n");
        return -1;
    }

    /* CMD7 selects the card, R1B; the HAL waits for DAT0 release */
    err = miyoo_sd_cmd(SD_CMD_SELECT_CARD, (uint32_t)_card.rca << 16, EV_R1B);
    if(err != EV_STS_OK) {
        printf("[SD] CMD7 fail: 0x%X\n", err);
        return -1;
    }

    /* Confirm TRAN with READY_FOR_DATA before configuring the bus */
    if(miyoo_sd_wait_state(SD_STATE_TRAN, SD_INIT_STATE_TIMEOUT_MS) != 0) {
        printf("[SD] wait TRAN fail\n");
        return -1;
    }

    /* ACMD6 to 4-bit; on failure continue with 1-bit (the host defaults to
     * 1-bit) */
    if(miyoo_sd_acmd(_card.rca, SD_ACMD_SET_BUS_WIDTH, 2, EV_R1) == EV_STS_OK) {
        _card.bus_4bit = 1;
        Hal_SDMMC_SetDataWidth(ip, EV_BUS_4BITS);
    }

    /* Block length fixed at 512 (ignored by SDHC, required by SDSC) */
    if(miyoo_sd_cmd(SD_CMD_SET_BLOCKLEN, 512, EV_R1) != EV_STS_OK) {
        printf("[SD] CMD16 fail\n");
        return -1;
    }

    /* CMD6 high-speed switch (only tried on v2 cards; failure leaves the
     * default speed usable) */
    if(_card.is_v2 && miyoo_sd_try_switch_hs() == 0) {
        _card.is_hs = 1;
        Hal_SDMMC_SetBusTiming(ip, EV_BUS_HS);
    } else {
        Hal_SDMMC_SetBusTiming(ip, EV_BUS_DEF);
    }

    /* Confirm the card state again after the switch */
    if(miyoo_sd_wait_state(SD_STATE_TRAN, SD_INIT_STATE_TIMEOUT_MS) != 0) {
        printf("[SD] wait TRAN after switch fail\n");
        return -1;
    }

    _card.inited = 1;
    return 0;
}

static void miyoo_sd_recover(void) {
    miyoo_sd_ra_invalidate();
    (void)miyoo_sd_card_init();
}

static int miyoo_sd_should_retry(RspErrEmType err) {
        ErrGrpEmType group;

        if(err == EV_STS_OK)
                return 0;
        group = Hal_SDMMC_ErrGroup(err);
        return (group == EV_EGRP_TOUT) || (group == EV_EGRP_COMM);
}

/* ------------------------------------------------------------------
 * CMD18 multi-block read: one DMA segment reads blk_cnt sectors into buf.
 * Once the read completes (success or failure) CMD12 must stop the
 * transfer, otherwise the card stays in send-data state holding the DAT
 * lines; CMD13 then confirms the card is back in TRAN, and if not the card
 * is fully reinitialized and the read retried.
 * ------------------------------------------------------------------ */
static RspErrEmType miyoo_sd_read_multi(uint32_t sector, uint16_t blk_cnt,
                volatile uint8_t *buf) {
        RspStruct *rsp = 0;
        RspErrEmType data_err = EV_OTHER_ERR;
        uint32_t attempt;

        for(attempt = 0; attempt < MIYOO_SD_RETRY_COUNT; attempt++) {
                /* SDHC/SDXC use block addressing, SDSC byte addressing */
                uint32_t addr = _card.is_sdhc ? sector : sector * 512U;

                rsp = _SDMMC_DATAReq(0, SD_CMD_READ_MULTIPLE_BLOCK, addr,
                                blk_cnt, 512, EV_DMA, buf);
                data_err = rsp->eErrCode;

                miyoo_sd_cmd(SD_CMD_STOP_TRANSMISSION, 0, EV_R1B);

                if(data_err == EV_STS_OK) {
                        if(miyoo_sd_wait_state(SD_STATE_TRAN,
                                        SD_STOP_STATE_TIMEOUT_MS) == 0)
                                return EV_STS_OK;
                        data_err = EV_STS_DAT0_BUSY; /* stuck outside TRAN, retryable */
                }

                if(!miyoo_sd_should_retry(data_err))
                        return data_err;
                miyoo_sd_recover();
        }

        return data_err;
}

uint16_t SDMMC_Init(uint8_t u8Slot)
{
        (void)u8Slot;
        return miyoo_sd_card_init() == 0 ? 0 : (uint16_t)EV_OTHER_ERR;
}

int32_t sd_init(void) {
        return SDMMC_Init(0) == 0 ? 0 : -1;
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

/* Fill the read-ahead window with one CMD18 (at most 128 sectors, exactly
 * filling the bounce buffer) */
static int32_t miyoo_sd_fill_ra_window(int32_t sector) {
        uint32_t window = miyoo_sd_pick_ra_window(sector);
        RspErrEmType err = miyoo_sd_read_multi((uint32_t)sector,
                        (uint16_t)window, _sector_buf);

        if(err != EV_STS_OK)
                return err;

        _ra_start_sector = sector;
        _ra_sector_count = window;
        return 0;
}

int32_t sd_dev_read(int32_t sector) {
        if(sector < 0)
                return -1;

        /* Fallback: initialize automatically when the read path is entered
         * without sd_init() */
        if(!_card.inited && miyoo_sd_card_init() != 0)
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

/*
 * Bulk read: split by bounce capacity, one CMD18 per chunk straight into the
 * bounce buffer, then copied out. Going through the bounce buffer keeps DMA
 * coherent (the bounce buffer is in the dev-mapped region, uncached).
 */
int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
    uint8_t* out = (uint8_t*)buf;

    if(buf == 0 || count == 0)
        return -1;

    if(!_card.inited && miyoo_sd_card_init() != 0)
        return -1;

    miyoo_sd_ra_invalidate();

    while(count > 0) {
        uint32_t chunk = (count > MIYOO_SD_BOUNCE_SECTORS) ?
                        MIYOO_SD_BOUNCE_SECTORS : count;

        if(miyoo_sd_read_multi((uint32_t)sector, (uint16_t)chunk,
                        _sector_buf) != EV_STS_OK)
            return -1;

        memcpy(out, _sector_buf, chunk * 512U);
        sector += (int32_t)chunk;
        out += chunk * 512U;
        count -= chunk;
    }
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
