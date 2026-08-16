/*
 * miyoo kernel-side SD driver.
 *
 * 按 system 侧的读流程改写：
 *   1) 完整初始化 SD 卡（不再假设前一阶段已把卡配置好）：
 *        CMD0 -> CMD8 -> ACMD41(轮询 OCR) -> CMD2 -> CMD3 -> CMD7(R1B busy)
 *        -> CMD13 轮询直到 TRAN -> ACMD6(4-bit) -> CMD16(512)
 *      每一步都校验响应，结束时用 CMD13 确认卡处于 TRAN 且 READY_FOR_DATA，
 *      保证进入读路径前卡状态正确。数据读取出错时整体重走该初始化恢复。
 *   2) CMD6 (SWITCH, arg 0x80FFFFF1) 切到 High-Speed：
 *        DMA 读回 64 字节 switch status，校验 group1 支持位与切换结果位，
 *        成功后 host 侧切 EV_BUS_HS 采样时序；失败/不支持则保持默认速度。
 *      注意：host 侧 SD 时钟暂维持 boot 阶段的 8MHz 不变（system 的时钟
 *      设置寄存器待后续从固件反查后单独加入），本次只做卡侧切换 + HS 时序。
 *
 * 读路径：CMD18 真正的多块读 + 单段 DMA（替代 CMD17 逐块循环）。
 *   - 预读窗口（4 -> 128 扇区）一次 CMD18 填满 bounce buffer；
 *   - 读完无论成败都发 CMD12 停止传输（R1B 等 DAT0），再 CMD13 确认回 TRAN；
 *   - sd_dev_read_blocks 按 bounce 容量分块 CMD18 直读后拷出。
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

/* mode=switch(1), group1(access mode)=HS(1), 其余 group 保持(0xF) */
#define SD_CMD6_ARG_HS      0x80FFFFF1U
#define SD_SWITCH_STS_BYTES 64U

#define SD_INIT_ACMD41_TIMEOUT_MS 2000U
#define SD_INIT_STATE_TIMEOUT_MS  500U
#define SD_STOP_STATE_TIMEOUT_MS  100U

static uint8_t *_sector_buf = (uint8_t*)MIYOO_SD_BOUNCE_VIRT;

typedef struct {
    int inited;    /* 完整初始化流程已成功走完 */
    int is_v2;     /* CMD8 有响应: SD 规范 v2+ 卡 */
    int is_sdhc;   /* OCR CCS: SDHC/SDXC，块寻址 */
    int is_hs;     /* CMD6 高速切换已被卡接受 */
    int bus_4bit;  /* ACMD6 4-bit 已被卡接受 */
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

/* R1/R3/R6/R7 的 32bit 负载: token[1..4]，MSB 在前 */
static inline uint32_t sd_rsp32(const RspStruct *rsp) {
    return ((uint32_t)rsp->u8ArrRspToken[1] << 24) |
           ((uint32_t)rsp->u8ArrRspToken[2] << 16) |
           ((uint32_t)rsp->u8ArrRspToken[3] << 8)  |
           (uint32_t)rsp->u8ArrRspToken[4];
}

/* ------------------------------------------------------------------
 * 预读窗口管理
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
 * 命令收发包装
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
 * 初始化子步骤
 * ------------------------------------------------------------------ */

/* CMD8 探测 SD v2；无响应/CRC 错都按 v1 传统卡继续走 */
static int miyoo_sd_probe_v2(void) {
    RspErrEmType err = miyoo_sd_cmd(SD_CMD_SEND_IF_COND, 0x1AAU, EV_R7);
    RspStruct *rsp = Hal_SDMMC_GetRspToken(MIYOO_SD_IP);

    if(err != EV_STS_OK)
        return 0;
    return (sd_rsp32(rsp) & 0xFFFU) == 0x1AAU;
}

/* ACMD41 轮询直到 OCR busy 位置起（卡上电完成） */
static int miyoo_sd_init_ocr(uint32_t arg) {
    uint32_t elapsed = 0;

    while(elapsed < SD_INIT_ACMD41_TIMEOUT_MS) {
        RspErrEmType err = miyoo_sd_acmd(0, SD_ACMD_SD_SEND_OP_COND, arg, EV_R3);
        RspStruct *rsp = Hal_SDMMC_GetRspToken(MIYOO_SD_IP);
        uint32_t ocr = sd_rsp32(rsp);

        if(err == EV_STS_OK && (ocr & SD_OCR_BUSY)) {
            if(!(ocr & SD_OCR_VDD_27_36))
                return -1; /* 电压范围不匹配 */
            _card.is_sdhc = (ocr & SD_OCR_CCS) ? 1 : 0;
            return 0;
        }
        sd_msleep(1);
        elapsed++;
    }
    return -1;
}

/* CMD13 轮询，直到 CURRENT_STATE == 目标状态且 READY_FOR_DATA */
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
 * CMD6 切 High-Speed。64 字节 switch status 走 DMA 读回 bounce buffer
 * 头部（dev-mapped 区，DMA 一致，无 cache 问题）。
 * 校验点（大端字节序）：
 *   byte[13] bit1  -> group1 function1 (HS) 支持位
 *   byte[16] 3:0   -> group1 实际切换结果，==1 才算成功
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
        return -1; /* 卡不支持 HS */
    if((sts[16] & 0x0F) != 0x01)
        return -1; /* 切换未生效 */
    return 0;
}

/* ------------------------------------------------------------------
 * 完整卡初始化（init 与运行期错误恢复共用）
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

    /* 时钟由 boot 阶段保持开启，这里补一段空转时钟满足 >=74 clocks 要求 */
    Hal_SDMMC_ClkCtrl(ip, TRUE, 1);

    /* CMD0: 回到 idle（允许失败重试几次，覆盖热重启场景） */
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

    /* CMD7 选中卡，R1B；HAL 内部会等 DAT0 释放 */
    err = miyoo_sd_cmd(SD_CMD_SELECT_CARD, (uint32_t)_card.rca << 16, EV_R1B);
    if(err != EV_STS_OK) {
        printf("[SD] CMD7 fail: 0x%X\n", err);
        return -1;
    }

    /* 确认进入 TRAN 且 READY_FOR_DATA，状态正确后再配置总线 */
    if(miyoo_sd_wait_state(SD_STATE_TRAN, SD_INIT_STATE_TIMEOUT_MS) != 0) {
        printf("[SD] wait TRAN fail\n");
        return -1;
    }

    /* ACMD6 切 4-bit；失败则保持 1-bit 继续（host 侧默认已是 1-bit） */
    if(miyoo_sd_acmd(_card.rca, SD_ACMD_SET_BUS_WIDTH, 2, EV_R1) == EV_STS_OK) {
        _card.bus_4bit = 1;
        Hal_SDMMC_SetDataWidth(ip, EV_BUS_4BITS);
    }

    /* 块长度固定 512（SDHC 会忽略，SDSC 必须） */
    if(miyoo_sd_cmd(SD_CMD_SET_BLOCKLEN, 512, EV_R1) != EV_STS_OK) {
        printf("[SD] CMD16 fail\n");
        return -1;
    }

    /* CMD6 高速切换（只对 v2 卡尝试；失败不影响默认速度可用性） */
    if(_card.is_v2 && miyoo_sd_try_switch_hs() == 0) {
        _card.is_hs = 1;
        Hal_SDMMC_SetBusTiming(ip, EV_BUS_HS);
    } else {
        Hal_SDMMC_SetBusTiming(ip, EV_BUS_DEF);
    }

    /* 切换后再次确认卡状态正确 */
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
 * CMD18 多块读：单段 DMA 读 blk_cnt 个扇区到 buf。
 * 读完成（无论成败）必须 CMD12 停止传输，否则卡会停在 send-data
 * 状态占住 DAT 线；随后 CMD13 确认卡回到 TRAN，状态不对则整体
 * 重初始化再重试。
 * ------------------------------------------------------------------ */
static RspErrEmType miyoo_sd_read_multi(uint32_t sector, uint16_t blk_cnt,
                volatile uint8_t *buf) {
        RspStruct *rsp = 0;
        RspErrEmType data_err = EV_OTHER_ERR;
        uint32_t attempt;

        for(attempt = 0; attempt < MIYOO_SD_RETRY_COUNT; attempt++) {
                /* SDHC/SDXC 块寻址，SDSC 字节寻址 */
                uint32_t addr = _card.is_sdhc ? sector : sector * 512U;

                rsp = _SDMMC_DATAReq(0, SD_CMD_READ_MULTIPLE_BLOCK, addr,
                                blk_cnt, 512, EV_DMA, buf);
                data_err = rsp->eErrCode;

                miyoo_sd_cmd(SD_CMD_STOP_TRANSMISSION, 0, EV_R1B);

                if(data_err == EV_STS_OK) {
                        if(miyoo_sd_wait_state(SD_STATE_TRAN,
                                        SD_STOP_STATE_TIMEOUT_MS) == 0)
                                return EV_STS_OK;
                        data_err = EV_STS_DAT0_BUSY; /* 停在非 TRAN 态，可重试 */
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

/* 一次 CMD18 填满预读窗口（最大 128 扇区，恰好用满 bounce buffer） */
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

        /* 兜底：未经 sd_init() 直接进入读路径时自动完成初始化 */
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
 * 批量读：按 bounce 容量分块，每块一次 CMD18 直读 bounce buffer 后拷出。
 * 走 bounce 中转是为了 DMA 一致性（bounce 在 dev-mapped 区，无 cache）。
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
