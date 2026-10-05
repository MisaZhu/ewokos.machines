#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>

#include <arch/miyoo/sd.h>
#include "sdmmc.h"
static uint8_t *_sector_buf;
static AdmaDescStruct *_adma_desc;
static SDMMCBusWidthEmType _active_bus_width = EV_BUS_4BITS;
static BusTimingEmType _active_bus_timing = EV_BUS_DEF;
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
#define MIYOO_SD_RETRY_DELAY_US 10000U
#define MIYOO_SD_RECOVER_SUCCESS_STREAK 32U
#define MIYOO_SD_SYSTEM_SAFE_CHUNK 1U
#define MIYOO_SD_SWITCH_STATUS_SIZE 64U
#define MIYOO_SD_CHECK_HS_ARG 0x00FFFFF1U
#define MIYOO_SD_SWITCH_HS_ARG 0x80FFFFF1U
/*
 * Sectors per CMD18 (32KB). The usable bounce buffer ceiling is 127 sectors
 * (sector 128 holds the ADMA descriptor); 64 keeps it a power of two and
 * matches the doubling ramp-up in miyoo_sd_note_success. On error it still
 * falls back to SAFE_CHUNK and recovers gradually, leaving every
 * fault-tolerance timing unchanged.
 */
#define MIYOO_SD_SYSTEM_FAST_CHUNK  64U


static RspStruct *_SDMMC_DATAReq(uint8_t u8Slot, uint8_t u8Cmd, uint32_t u32Arg,
        uint16_t u16BlkCnt, uint16_t u16BlkSize, TransEmType eTransType,
        volatile uint8_t *pu8Buf);
static int32_t miyoo_sd_write_one(int32_t sector, const uint8_t* src);
static RspErrEmType miyoo_sd_cmd12(IPEmType eIP);

static inline uint32_t miyoo_sd_dma_addr(volatile uint8_t *buf) {
    ewokos_addr_t phy = (ewokos_addr_t)buf - (ewokos_addr_t)MIYOO_SD_DMA_VIRT_OFFSET;
    return Hal_CARD_TransMIUAddr((uint32_t)phy);
}

static void miyoo_sd_apply_bus_width(SDMMCBusWidthEmType bus_width) {
    _active_bus_width = bus_width;
    Hal_SDMMC_SetDataWidth(EV_IP_FCIE1, _active_bus_width);
    Hal_SDMMC_SetBusTiming(EV_IP_FCIE1, _active_bus_timing);
    /* Sampling mode does not change the real clock; command spacing still
     * uses the current 8MHz configuration. */
    Hal_SDMMC_SetNrcDelay(EV_IP_FCIE1, MIYOO_SD_REAL_CLK_HZ);
}

static void miyoo_sd_note_success(void) {
    if(_active_chunk_sectors == _fast_chunk_sectors) {
        _stable_successes = 0;
        return;
    }

    if(++_stable_successes >= MIYOO_SD_RECOVER_SUCCESS_STREAK) {
        _stable_successes = 0;
        _active_chunk_sectors <<= 1;
        if(_active_chunk_sectors > _fast_chunk_sectors)
            _active_chunk_sectors = _fast_chunk_sectors;
    }
}

static void miyoo_sd_note_retryable_error(void) {
    _stable_successes = 0;
    _active_chunk_sectors = MIYOO_SD_SYSTEM_SAFE_CHUNK;
    /* Only shrink the batch. Without card-side negotiation the host bus
     * width or sampling mode must not be changed on its own. */
}

/*
 * Multi-block transfer failures are usually caused by DMA/ADMA
 * controller quirks or CMD18 timing, NOT by signal integrity.
 * Only reduce the chunk size and leave bus width and HS timing
 * untouched, avoiding a permanent speed downgrade from a single
 * transient multi-block error.
 */
static void miyoo_sd_note_chunk_error(void) {
    _stable_successes = 0;
    _active_chunk_sectors = MIYOO_SD_SYSTEM_SAFE_CHUNK;
}

static RspErrEmType miyoo_sd_recover(void) {
    Hal_SDMMC_Reset(EV_IP_FCIE1);
    sdmmc_init();
    miyoo_sd_apply_bus_width(_active_bus_width);
    /* A host reset does not reset the card; stop any lingering data
     * transfer and wait for DAT0 to release. */
    return miyoo_sd_cmd12(EV_IP_FCIE1);
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
    RspErrEmType err = EV_OTHER_ERR;
    uint32_t attempt;

    for(attempt = 0; attempt < MIYOO_SD_RETRY_COUNT; attempt++) {
        RspStruct *rsp = _SDMMC_DATAReq(0, cmd, sector, blk_cnt, blk_size, trans_type, buf);
        /* The response lives in HAL shared storage; recovery or the next
         * command will overwrite it. */
        err = rsp->eErrCode;
        if(err == EV_STS_OK) {
            miyoo_sd_note_success();
            return EV_STS_OK;
        }
        if(!miyoo_sd_should_retry(err))
            return err;
        miyoo_sd_note_retryable_error();
        if(miyoo_sd_recover() != EV_STS_OK)
            return err;
        if(attempt + 1U < MIYOO_SD_RETRY_COUNT)
            usleep(MIYOO_SD_RETRY_DELAY_US);
    }

    return err;
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

/*
 * The CMD6 check and switch must both receive the 64-byte status; an MBR
 * signature cannot stand in for it. Probe only at init time so a runtime
 * probe does not clobber the bounce buffer shared by reads and writes.
 * Returns 1 if switched, 0 if unsupported or busy, -1 on transfer/status
 * error. Only the HS sampling mode is set here; the real SD clock is
 * unchanged.
 */
static int miyoo_sd_try_high_speed(void) {
    RspStruct *rsp;

    memset(_sector_buf, 0, MIYOO_SD_SWITCH_STATUS_SIZE);
    rsp = _SDMMC_DATAReq(0, 6, MIYOO_SD_CHECK_HS_ARG, 1,
            MIYOO_SD_SWITCH_STATUS_SIZE, EV_DMA, _sector_buf);
    if(rsp->eErrCode != EV_STS_OK)
        return -1;
    if((_sector_buf[13] & 0x02U) == 0)
        return 0;
    if(_sector_buf[17] >= 1U && (_sector_buf[29] & 0x02U) != 0)
        return 0;

    memset(_sector_buf, 0, MIYOO_SD_SWITCH_STATUS_SIZE);
    rsp = _SDMMC_DATAReq(0, 6, MIYOO_SD_SWITCH_HS_ARG, 1,
            MIYOO_SD_SWITCH_STATUS_SIZE, EV_DMA, _sector_buf);
    if(rsp->eErrCode != EV_STS_OK)
        return -1;
    if((_sector_buf[13] & 0x02U) == 0 || (_sector_buf[16] & 0x0FU) != 1U)
        return -1;

    _active_bus_timing = EV_BUS_HS;
    miyoo_sd_apply_bus_width(_active_bus_width);
    return 1;
}

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
    if(boot_bus_width != EV_BUS_1BIT && boot_bus_width != EV_BUS_4BITS)
        return -1;
    sdmmc_init();
    _active_bus_timing = EV_BUS_DEF;
    _fast_chunk_sectors = MIYOO_SD_SYSTEM_FAST_CHUNK;
    _active_chunk_sectors = _fast_chunk_sectors;
    _stable_successes = 0;
    miyoo_sd_apply_bus_width(boot_bus_width);

    /* End the card-side transfer first after an optional probe failure; a
     * failed recovery must not masquerade as a successful init. */
    int hs = miyoo_sd_try_high_speed();
    if(hs < 0) {
        if(miyoo_sd_recover() != EV_STS_OK)
            return -1;
    }
    else if(hs > 0) {
        klog("miyoo_sd: HS sampling enabled, SD clock unchanged\n");
    }
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
    if(buf == NULL)
        return -1;
    return miyoo_sd_write_one(sector, (const uint8_t*)buf);
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
     * FCIE5 multi-block reads must go through ADMA: in DMA mode
     * JOB_BLK_CNT equals the block count directly, but
     * Hal_SDMMC_SendCmdAndWaitProcess triggers R_DATA_END by JOB_BLK_CNT and
     * the controller hardware will not auto-split 8 blocks per the CMD18
     * block count; only u32_JobCnt=chunks in the ADMA descriptor drives
     * multi-block continuation.
     *
     * Retrying is counterproductive here: resending the same CMD18 in rdata
     * state always times out, and miyoo_sd_run_request's 5x2s cost would
     * blow through the upper filesystem's read timeout. On failure let the
     * outer miyoo_sd_read_blocks take the single-block fallback path.
     */
    if(count == 1)
        return miyoo_sd_run_request(17, sector, 1, 512, EV_ADMA, buf);

    rsp = _SDMMC_DATAReq(0, 18, sector, (uint16_t)count, 512, EV_ADMA, buf);
    err = rsp->eErrCode;

    /*
     * CMD18 leaves the card in rdata state with DAT0 busy; the FCIE5 HAL
     * does not auto-issue CMD12, so we must stop the transfer explicitly
     * before the next command (or the very next read will time out).
     * Send it even on failure: pull the card from rdata back to tran,
     * otherwise the later single-block fallback will also hang.
     */
    RspErrEmType stop_err = miyoo_sd_cmd12(EV_IP_FCIE1);
    if(err == EV_STS_OK)
        err = stop_err;

    if(err == EV_STS_OK)
        miyoo_sd_note_success();
    return err;
}

static RspErrEmType miyoo_sd_try_write_multi(uint32_t sector, uint32_t count, const volatile uint8_t* buf) {
    RspStruct *rsp;
    RspErrEmType err;
    if(count == 1)
        return miyoo_sd_run_request(24, sector, 1, 512, EV_DMA, (volatile uint8_t*)buf);
    /*
     * Same as multi-block read: after a failed CMD25 the card stays in rcv
     * state, and resending the same CMD25 in place only times out
     * repeatedly (run_request's 5 blind retries are counterproductive
     * here). Single attempt; on failure let the outer layer use the
     * single-block fallback path.
     */
    rsp = _SDMMC_DATAReq(0, 25, sector, (uint16_t)count, 512, EV_DMA, (volatile uint8_t*)buf);
    err = rsp->eErrCode;
    /* Same reasoning as the read path: release the card from rcv state
     * (CMD12 is R1b, so it also waits out the programming busy). */
    RspErrEmType stop_err = miyoo_sd_cmd12(EV_IP_FCIE1);
    if(err == EV_STS_OK)
        err = stop_err;
    if(err == EV_STS_OK)
        miyoo_sd_note_success();
    return err;
}

/*
 * Single-sector write. No read-back verification after writing: the data
 * phase CRC, on-card ECC and post-write busy wait are the integrity
 * guarantee; a read-back would only pay a full extra read per write to
 * cover historical driver timing problems.
 */
static int32_t miyoo_sd_write_one(int32_t sector, const uint8_t* src) {
    RspErrEmType err;

    memcpy(_sector_buf, src, 512U);
    err = miyoo_sd_run_request(24, sector, 1, 512, EV_DMA, _sector_buf);
    if(err != EV_STS_OK)
        return err;
    return 0;
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
            /* multi-block failed, fall back to single-block retry */
            miyoo_sd_note_chunk_error();
            if(miyoo_sd_recover() != EV_STS_OK)
                return err;
        }

        /* single-block fallback */
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
            /* multi-block failed, fall back to single-block retry */
            miyoo_sd_note_chunk_error();
            if(miyoo_sd_recover() != EV_STS_OK)
                return err;
        }

        /* single-block fallback */
        err = miyoo_sd_write_one(sector, src);
        if(err != 0)
            return err;
        src += 512U;
        sector++;
        count--;
    }
    return 0;
}
