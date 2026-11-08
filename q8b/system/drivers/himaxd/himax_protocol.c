#include "himax_protocol.h"

#include <string.h>

/* Single-threaded: the daemon, the diagnostic tool and the host tests all
 * drive one IC at a time.  Keeping the transfer buffers static avoids a
 * 10 KiB stack frame in a 4 KiB-ish EwokOS thread. */
static uint8_t _tx[HIMAX_XFER_MAX];
static uint8_t _rx[HIMAX_XFER_MAX];

static void wait_ms(himax_dev_t* dev, unsigned ms) {
    if (dev->delay_us)
        dev->delay_us(dev->hw_ctx, ms * 1000U);
}

static void pin_reset(himax_dev_t* dev) {
    /* reset-gpios is active low: assert for 20 ms, then release for 50 ms. */
    if (!dev->reset)
        return;
    dev->reset(dev->hw_ctx, true);
    wait_ms(dev, 20);
    dev->reset(dev->hw_ctx, false);
    wait_ms(dev, 50);
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* [0xf3][cmd][dummy] then payload; retried like the Linux driver. */
static int bus_read(himax_dev_t* dev, uint8_t cmd, uint8_t* buf, size_t len) {
    size_t total = HIMAX_BUS_R_HLEN + len;
    int ret = -1;

    if (total > sizeof(_tx) || !dev->bus.xfer) {
        dev->stage = "spi-read-bounds";
        return -1;
    }
    memset(_tx, 0, total);
    _tx[0] = HIMAX_SPI_FUNCTION_READ;
    _tx[1] = cmd;
    for (unsigned i = 0; i < HIMAX_BUS_RETRY; i++) {
        if (dev->bus.xfer(dev->bus.ctx, _tx, _rx, total) == 0) {
            ret = 0;
            break;
        }
    }
    if (ret != 0) {
        dev->bus_errors++;
        dev->stage = "spi-read-failed";
        return -1;
    }
    memcpy(buf, _rx + HIMAX_BUS_R_HLEN, len);
    return 0;
}

/* [0xf2][cmd] then optional four address bytes, then the payload. */
static int bus_write(himax_dev_t* dev, uint8_t cmd, const uint8_t* addr,
        const uint8_t* data, size_t len) {
    size_t total = HIMAX_BUS_W_HLEN + (addr ? 4U : 0U) + len;
    size_t pos = HIMAX_BUS_W_HLEN;

    if (total > sizeof(_tx) || !dev->bus.xfer) {
        dev->stage = "spi-write-bounds";
        return -1;
    }
    memset(_tx, 0, total);
    _tx[0] = HIMAX_SPI_FUNCTION_WRITE;
    _tx[1] = cmd;
    if (addr) {
        memcpy(_tx + pos, addr, 4);
        pos += 4;
    }
    if (len) {
        if (!data) {
            dev->stage = "spi-write-no-data";
            return -1;
        }
        memcpy(_tx + pos, data, len);
    }
    if (dev->bus.xfer(dev->bus.ctx, _tx, NULL, total) != 0) {
        dev->bus_errors++;
        dev->stage = "spi-write-failed";
        return -1;
    }
    return 0;
}

/* CONTI selects streaming access; INCR4 auto-increments the address every
 * four bytes.  The trailing restore call mirrors the Linux driver exactly. */
static int set_burst_mode(himax_dev_t* dev, bool auto_add4) {
    uint8_t conti = HIMAX_AHB_CMD_CONTI;
    uint8_t incr4 = HIMAX_AHB_CMD_INCR4;

    if (auto_add4)
        incr4 |= HIMAX_AHB_CMD_INCR4_ADD4;
    if (bus_write(dev, HIMAX_AHB_ADDR_CONTI, NULL, &conti, 1) != 0)
        return -1;
    return bus_write(dev, HIMAX_AHB_ADDR_INCR4, NULL, &incr4, 1);
}

int himax_reg_write(himax_dev_t* dev, uint32_t addr, const uint8_t* data,
        size_t len) {
    uint8_t target[4];
    int ret;

    target[0] = (uint8_t)addr;
    target[1] = (uint8_t)(addr >> 8);
    target[2] = (uint8_t)(addr >> 16);
    target[3] = (uint8_t)(addr >> 24);

    if (set_burst_mode(dev, len > 4) != 0)
        return -1;
    ret = bus_write(dev, HIMAX_AHB_ADDR_BYTE_0, target, data, len);
    if (set_burst_mode(dev, len <= 4) != 0)
        return -1;
    return ret;
}

int himax_reg_read(himax_dev_t* dev, uint32_t addr, uint8_t* buf, size_t len) {
    uint8_t target[4];
    uint8_t direction = HIMAX_AHB_CMD_DIRECTION_READ;
    int ret;

    target[0] = (uint8_t)addr;
    target[1] = (uint8_t)(addr >> 8);
    target[2] = (uint8_t)(addr >> 16);
    target[3] = (uint8_t)(addr >> 24);

    if (set_burst_mode(dev, len > 4) != 0)
        return -1;
    if (bus_write(dev, HIMAX_AHB_ADDR_BYTE_0, target, NULL, 0) != 0)
        return -1;
    if (bus_write(dev, HIMAX_AHB_ADDR_DIRECTION, NULL, &direction, 1) != 0)
        return -1;
    dev->stage = "reg-read-data";
    ret = bus_read(dev, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf, len);
    if (set_burst_mode(dev, len <= 4) != 0)
        return -1;
    return ret;
}

/* The IC may be asleep; wake the bus, then confirm both burst registers. */
int himax_interface_on(himax_dev_t* dev) {
    uint8_t wake[4];
    uint8_t conti, incr4;

    dev->stage = "interface-wake";
    if (bus_read(dev, HIMAX_AHB_ADDR_RDATA_BYTE_0, wake, 4) != 0)
        return -1;
    for (unsigned i = 0; i < 10; i++) {
        dev->stage = "interface-burst-mode";
        if (set_burst_mode(dev, false) != 0)
            return -1;
        dev->stage = "interface-conti-check";
        if (bus_read(dev, HIMAX_AHB_ADDR_CONTI, &conti, 1) != 0)
            return -1;
        dev->stage = "interface-incr4-check";
        if (bus_read(dev, HIMAX_AHB_ADDR_INCR4, &incr4, 1) != 0)
            return -1;
        if (conti == HIMAX_AHB_CMD_CONTI && incr4 == HIMAX_AHB_CMD_INCR4)
            return 0;
        wait_ms(dev, 1);
    }
    dev->stage = "interface-no-response";
    return -1;
}

int himax_sense_off(himax_dev_t* dev, bool check_fw) {
    uint8_t data[4] = { 0, 0, 0, 0 };
    const uint8_t go_safe_mode[4] = { HIMAX_FW_STATE_GO_SAFEMODE, 0, 0, 0 };

    if (check_fw) {
        for (unsigned i = 0; i < 35; i++) {
            if (i == 0 || (data[0] != HIMAX_FW_STATE_GO_SAFEMODE &&
                    data[0] != HIMAX_FW_STATE_RE_INIT &&
                    data[0] != HIMAX_FW_STATE_IN_SAFEMODE)) {
                dev->stage = "sense-off-stop-fw";
                if (himax_reg_write(dev, HIMAX_REG_FW_CTRL,
                        go_safe_mode, 4) != 0)
                    return -1;
            }
            wait_ms(dev, 10);
            dev->stage = "sense-off-fw-status";
            if (himax_reg_read(dev, HIMAX_REG_FW_STATUS, data, 4) != 0)
                return -1;
            if (data[0] != HIMAX_FW_STATE_RUNNING)
                break;
            dev->stage = "sense-off-fw-ctrl";
            if (himax_reg_read(dev, HIMAX_REG_FW_CTRL, data, 4) != 0)
                return -1;
            if (data[0] == HIMAX_FW_STATE_IN_SAFEMODE)
                break;
        }
    }

    for (unsigned i = 0; i < 5; i++) {
        uint8_t password[2];
        password[0] = (uint8_t)(HIMAX_SAFE_MODE_PASSWORD & 0xffU);
        password[1] = (uint8_t)(HIMAX_SAFE_MODE_PASSWORD >> 8);
        dev->stage = "sense-off-enter-safe-mode";
        if (bus_write(dev, HIMAX_AHB_ADDR_PSW_LB, NULL, password, 2) != 0)
            return -1;
        dev->stage = "sense-off-safe-mode-status";
        if (himax_reg_read(dev, HIMAX_REG_FW_STATUS, data, 4) != 0)
            return -1;
        if (data[0] == HIMAX_FW_STATE_SAFE_MODE) {
            dev->stage = "sense-off-safe-mode";
            return 0;
        }
        wait_ms(dev, 10);
        pin_reset(dev);
    }
    dev->stage = "sense-off-failed";
    return -1;
}

int himax_sense_on(himax_dev_t* dev, bool software_reset) {
    const uint8_t re_init[4] = { 0, 0, 0, 0 };

    dev->stage = "sense-on-interface";
    if (himax_interface_on(dev) != 0)
        return -1;
    dev->stage = "sense-on-fw-reinit";
    if (himax_reg_write(dev, HIMAX_REG_FW_CTRL, re_init, 4) != 0)
        return -1;
    wait_ms(dev, 10);
    if (!software_reset) {
        pin_reset(dev);
    } else {
        const uint8_t leave[2] = { 0, 0 };
        dev->stage = "sense-on-leave-safe-mode";
        if (bus_write(dev, HIMAX_AHB_ADDR_PSW_LB, NULL, leave, 2) != 0)
            return -1;
    }
    return 0;
}

int himax_chip_detect(himax_dev_t* dev) {
    uint8_t data[4];

    dev->stage = "detect-reset";
    pin_reset(dev);
    dev->stage = "detect-interface";
    if (himax_interface_on(dev) != 0)
        return -1;
    dev->stage = "detect-sense-off";
    if (himax_sense_off(dev, false) != 0)
        return -1;

    for (unsigned i = 0; i < 5; i++) {
        dev->stage = "detect-ic-id";
        if (himax_reg_read(dev, HIMAX_REG_IC_ID, data, 4) != 0)
            return -1;
        dev->ic_id = le32(data) >> 8;
        if (dev->ic_id == HIMAX_IC_ID) {
            dev->stage = "detect-ok";
            return 0;
        }
    }
    dev->stage = "detect-wrong-ic";
    return -1;
}

int himax_disable_fw_reload(himax_dev_t* dev) {
    /* 0x9aa9: no flash; 0x5aa5: flash present, do not reload; 0: reload. */
    const uint8_t has_flash[4] = {
        (uint8_t)(HIMAX_FLASH_RELOAD_HAS_FLASH & 0xffU),
        (uint8_t)(HIMAX_FLASH_RELOAD_HAS_FLASH >> 8), 0, 0
    };

    dev->stage = "fw-reload-disable";
    return himax_reg_write(dev, HIMAX_DSRAM_FLASH_RELOAD, has_flash, 4);
}

static int assign_sorting_mode(himax_dev_t* dev, const uint8_t* value) {
    uint8_t readback[4];

    for (unsigned i = 0; i < 3; i++) {
        dev->stage = "init-sorting-write";
        if (himax_reg_write(dev, HIMAX_DSRAM_SORTING_MODE_EN, value, 4) != 0)
            return -1;
        wait_ms(dev, 1);
        dev->stage = "init-sorting-read";
        if (himax_reg_read(dev, HIMAX_DSRAM_SORTING_MODE_EN, readback, 4) != 0)
            return -1;
        if (memcmp(readback, value, 4) == 0)
            return 0;
    }
    dev->stage = "init-sorting-mismatch";
    return -1;
}

int himax_power_on_init(himax_dev_t* dev, bool raw_output) {
    const uint8_t zero[4] = { 0, 0, 0, 0 };
    const uint8_t one[4] = { 1, 0, 0, 0 };
    const uint8_t raw_out[4] = { HIMAX_RAW_OUT_SELECT, 0, 0, 0 };
    uint8_t data[4];
    bool reloaded = false;

    dev->stage = "init-sorting-mode";
    if (assign_sorting_mode(dev, zero) != 0)
        return -1;
    dev->stage = "init-nframe";
    if (himax_reg_write(dev, HIMAX_DSRAM_SET_NFRAME, one, 4) != 0)
        return -1;
    dev->stage = "init-reload-status";
    if (himax_reg_write(dev, HIMAX_DSRAM_2ND_FLASH_RELOAD, zero, 4) != 0)
        return -1;
    dev->stage = "init-sense-on";
    if (himax_sense_on(dev, false) != 0)
        return -1;

    for (unsigned i = 0; i < 30; i++) {
        dev->stage = "init-wait-fw-reload";
        if (himax_reg_read(dev, HIMAX_DSRAM_2ND_FLASH_RELOAD, data, 4) != 0)
            return -1;
        if (le32(data) == HIMAX_DSRAM_FW_RELOAD_DONE) {
            reloaded = true;
            break;
        }
        wait_ms(dev, 10);
    }
    if (!reloaded) {
        dev->stage = "init-fw-reload-timeout";
        return -1;
    }
    if (raw_output) {
        dev->stage = "init-raw-out";
        if (himax_reg_write(dev, HIMAX_DSRAM_RAW_OUT_SEL, raw_out, 4) != 0)
            return -1;
    }
    dev->stage = "init-ready";
    return 0;
}

int himax_probe(himax_dev_t* dev) {
    dev->ic_id = 0;
    dev->bus_errors = 0;
    if (himax_chip_detect(dev) != 0)
        return -1;
    if (himax_disable_fw_reload(dev) != 0)
        return -1;
    return himax_power_on_init(dev, true);
}

int himax_probe_events(himax_dev_t* dev) {
    dev->ic_id = 0;
    dev->bus_errors = 0;
    if (himax_chip_detect(dev) != 0)
        return -1;
    if (himax_disable_fw_reload(dev) != 0)
        return -1;
    return himax_power_on_init(dev, false);
}

bool himax_frame_valid(const uint8_t* stream) {
    const uint16_t* grid;
    unsigned sane = 0;

    if (!stream)
        return false;
    if ((uint16_t)(stream[0] | ((uint16_t)stream[1] << 8)) !=
            HIMAX_STREAM_MARKER)
        return false;
    grid = (const uint16_t*)(stream + HIMAX_STREAM_HEADER_BYTES);
    for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
        if (grid[i] >= HIMAX_SANE_LOW && grid[i] <= HIMAX_SANE_HIGH)
            sane++;
    }
    return sane * 100U >= HIMAX_PIXELS * HIMAX_SANE_MIN_PERCENT;
}

int himax_read_frame(himax_dev_t* dev, uint8_t* stream) {
    dev->stage = "frame-read";
    /* Full-length read: an A/B register pair serves alternating records, so a
     * read that spans one panel update still returns a complete record.  The
     * shorter header+grid read broke that guarantee (measured ~78% rejects). */
    return bus_read(dev, HIMAX_AHB_ADDR_EVENT_STACK, stream,
            HIMAX_STREAM_BYTES);
}
