/*
 * Host test for the HX83121A protocol layer.
 *
 * A fake bus records every SPI transaction and emulates just enough of the
 * IC (AHB burst registers, firmware state, IC ID, DSRAM flags, frame stream)
 * to drive the whole bring-up sequence.  This verifies framing, ordering and
 * error handling without any hardware.
 */
#include <stdio.h>
#include <string.h>

#include "himax_protocol.h"

static int _failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            _failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define LOG_MAX 64

typedef struct {
    uint8_t tx[LOG_MAX];
    size_t len;
    int is_rx;
} xfer_log_t;

static xfer_log_t _log[LOG_MAX];
static unsigned _log_count;

typedef struct {
    /* emulated AHB state */
    uint8_t conti, incr4;
    uint32_t pending_addr;
    uint8_t direction;
    /* emulated IC registers and DSRAM flags */
    uint32_t ic_id_word;
    uint8_t fw_status;
    uint8_t fw_ctrl[4];
    uint32_t dsram_reload;
    uint32_t dsram_raw_sel;
    uint32_t dsram_sorting;
    uint32_t dsram_nframe;
    uint32_t dsram_flash_reload;
    bool fw_started;
    bool never_reload;
    uint8_t frame[HIMAX_STREAM_BYTES];
    /* behaviour switches */
    bool fail_next;
    int  fail_at;      /* fail the n-th transfer of the current test */
    bool silent;       /* IC ignores writes and reads back 0xff */
    char seq[512];
} fake_ic_t;

static fake_ic_t _ic;

static void seq_add(const char* tag) {
    size_t n = strlen(_ic.seq);
    if (_ic.seq[0] != 0)
        strcat(_ic.seq, ",");
    if (n + strlen(tag) + 2 < sizeof(_ic.seq))
        strcat(_ic.seq, tag);
}

static uint8_t word_byte(uint32_t word, uint32_t addr, uint32_t base) {
    uint32_t off = addr - base;
    if (off >= 4)
        return 0;
    return (uint8_t)(word >> (8 * off));
}

static uint8_t fake_read_byte(uint32_t addr) {
    if (addr >= HIMAX_REG_IC_ID && addr < HIMAX_REG_IC_ID + 4)
        return word_byte(_ic.ic_id_word, addr, HIMAX_REG_IC_ID);
    if (addr >= HIMAX_REG_FW_STATUS && addr < HIMAX_REG_FW_STATUS + 4)
        return (addr == HIMAX_REG_FW_STATUS) ? _ic.fw_status : 0;
    if (addr >= HIMAX_REG_FW_CTRL && addr < HIMAX_REG_FW_CTRL + 4)
        return _ic.fw_ctrl[addr - HIMAX_REG_FW_CTRL];
    if (addr >= HIMAX_DSRAM_2ND_FLASH_RELOAD &&
            addr < HIMAX_DSRAM_2ND_FLASH_RELOAD + 4) {
        /* The IC publishes the reload-complete word after the firmware is
         * started; the host only polls it. */
        if (_ic.fw_started && !_ic.never_reload && _ic.dsram_reload == 0)
            _ic.dsram_reload = HIMAX_DSRAM_FW_RELOAD_DONE;
        return word_byte(_ic.dsram_reload, addr, HIMAX_DSRAM_2ND_FLASH_RELOAD);
    }
    if (addr >= HIMAX_DSRAM_RAW_OUT_SEL && addr < HIMAX_DSRAM_RAW_OUT_SEL + 4)
        return word_byte(_ic.dsram_raw_sel, addr, HIMAX_DSRAM_RAW_OUT_SEL);
    if (addr >= HIMAX_DSRAM_SORTING_MODE_EN &&
            addr < HIMAX_DSRAM_SORTING_MODE_EN + 4)
        return word_byte(_ic.dsram_sorting, addr, HIMAX_DSRAM_SORTING_MODE_EN);
    if (addr >= HIMAX_DSRAM_SET_NFRAME && addr < HIMAX_DSRAM_SET_NFRAME + 4)
        return word_byte(_ic.dsram_nframe, addr, HIMAX_DSRAM_SET_NFRAME);
    if (addr >= HIMAX_DSRAM_FLASH_RELOAD &&
            addr < HIMAX_DSRAM_FLASH_RELOAD + 4)
        return word_byte(_ic.dsram_flash_reload, addr, HIMAX_DSRAM_FLASH_RELOAD);
    return 0;
}

static void fake_write_word(uint32_t addr, const uint8_t* data, size_t len) {
    uint32_t word = 0;
    for (size_t i = 0; i < len && i < 4; i++)
        word |= (uint32_t)data[i] << (8 * i);

    switch (addr) {
    case HIMAX_REG_FW_CTRL:
        memcpy(_ic.fw_ctrl, data, len < 4 ? len : 4);
        seq_add("fwctrl");
        if (word == HIMAX_FW_STATE_RE_INIT)
            _ic.fw_started = true;
        break;
    case HIMAX_DSRAM_2ND_FLASH_RELOAD:
        _ic.dsram_reload = word;
        break;
    case HIMAX_DSRAM_RAW_OUT_SEL:
        _ic.dsram_raw_sel = word;
        seq_add("rawout");
        break;
    case HIMAX_DSRAM_SORTING_MODE_EN:
        _ic.dsram_sorting = word;
        break;
    case HIMAX_DSRAM_SET_NFRAME:
        _ic.dsram_nframe = word;
        break;
    case HIMAX_DSRAM_FLASH_RELOAD:
        _ic.dsram_flash_reload = word;
        seq_add("flash-reload");
        break;
    default:
        break;
    }
}

static int fake_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, size_t len) {
    (void)ctx;
    if (_log_count < LOG_MAX) {
        xfer_log_t* e = &_log[_log_count++];
        e->len = len;
        e->is_rx = rx != NULL;
        memcpy(e->tx, tx, len < LOG_MAX ? len : LOG_MAX);
    }
    if (_ic.fail_next) {
        _ic.fail_next = false;
        return -1;
    }
    if (_ic.fail_at > 0 && (int)_log_count == _ic.fail_at) {
        _ic.fail_at = 0;
        return -1;
    }
    if (_ic.silent) {
        if (rx)
            memset(rx, 0xff, len);
        return 0;
    }
    if (tx[0] == HIMAX_SPI_FUNCTION_READ) {
        uint8_t cmd = tx[1];
        for (size_t i = 0; i < len; i++) {
            uint8_t value = 0;
            if (rx && i >= HIMAX_BUS_R_HLEN) {
                size_t n = i - HIMAX_BUS_R_HLEN;
                if (cmd == HIMAX_AHB_ADDR_CONTI)
                    value = n == 0 ? _ic.conti : 0;
                else if (cmd == HIMAX_AHB_ADDR_INCR4)
                    value = n == 0 ? _ic.incr4 : 0;
                else if (cmd == HIMAX_AHB_ADDR_RDATA_BYTE_0)
                    value = fake_read_byte(_ic.pending_addr + (uint32_t)n);
                else if (cmd == HIMAX_AHB_ADDR_EVENT_STACK)
                    value = n < sizeof(_ic.frame) ? _ic.frame[n] : 0;
            }
            if (rx)
                rx[i] = value;
        }
        return 0;
    }
    if (tx[0] == HIMAX_SPI_FUNCTION_WRITE) {
        uint8_t cmd = tx[1];
        const uint8_t* p = tx + 2;
        size_t n = len - 2;
        if (cmd == HIMAX_AHB_ADDR_CONTI) {
            _ic.conti = p[0];
        } else if (cmd == HIMAX_AHB_ADDR_INCR4) {
            _ic.incr4 = p[0];
        } else if (cmd == HIMAX_AHB_ADDR_DIRECTION) {
            _ic.direction = p[0];
        } else if (cmd == HIMAX_AHB_ADDR_PSW_LB) {
            seq_add("psw");
            if (n >= 2 && p[0] == (HIMAX_SAFE_MODE_PASSWORD & 0xff) &&
                    p[1] == (HIMAX_SAFE_MODE_PASSWORD >> 8))
                _ic.fw_status = HIMAX_FW_STATE_SAFE_MODE;
            else if (n >= 2 && p[0] == 0 && p[1] == 0)
                _ic.fw_status = HIMAX_FW_STATE_RUNNING;
        } else if (cmd == HIMAX_AHB_ADDR_BYTE_0) {
            uint32_t addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            _ic.pending_addr = addr;
            if (n >= 4)
                fake_write_word(addr, p + 4, n - 4);
        }
        return 0;
    }
    return -1;
}

static void reset(void* ctx, bool assert) {
    (void)ctx;
    seq_add(assert ? "rst-assert" : "rst-release");
    if (assert) {
        _ic.fw_status = HIMAX_FW_STATE_RUNNING;
        _ic.fw_started = false;
        _ic.dsram_reload = 0;
    } else {
        /* Releasing reset boots the IC; it then reloads its own flash. */
        _ic.fw_started = true;
    }
}

static void delay_us(void* ctx, unsigned usec) {
    (void)ctx; (void)usec;
}

static void ic_model_reset(void) {
    memset(&_ic, 0, sizeof(_ic));
    _ic.ic_id_word = HIMAX_IC_ID << 8;
    _ic.fw_status = HIMAX_FW_STATE_RUNNING;
    _ic.conti = HIMAX_AHB_CMD_CONTI;
    _ic.incr4 = HIMAX_AHB_CMD_INCR4;
    for (unsigned i = 0; i < HIMAX_STREAM_BYTES; i++)
        _ic.frame[i] = (uint8_t)i;
}

static void dev_init(himax_dev_t* dev) {
    memset(dev, 0, sizeof(*dev));
    dev->bus.xfer = fake_xfer;
    dev->reset = reset;
    dev->delay_us = delay_us;
    dev->stage = "startup";
}

static void test_register_read_framing(void) {
    himax_dev_t dev;
    uint8_t data[4] = { 0 };
    const uint8_t expected_addr[] = { 0x5c, 0x00, 0x00, 0x90 };

    ic_model_reset();
    dev_init(&dev);
    _ic.fw_ctrl[0] = 0x11;
    _log_count = 0;
    CHECK(himax_reg_read(&dev, HIMAX_REG_FW_CTRL, data, 4) == 0, "reg read");
    CHECK(_log_count == 7, "expected 7 transactions, got %u", _log_count);
    CHECK(_log[0].len == 3 && _log[0].tx[0] == 0xf2 &&
            _log[0].tx[1] == 0x13 && _log[0].tx[2] == 0x31,
            "conti write framing");
    CHECK(_log[1].len == 3 && _log[1].tx[0] == 0xf2 &&
            _log[1].tx[1] == 0x0d && _log[1].tx[2] == 0x10,
            "incr4 write framing");
    CHECK(_log[2].len == 6 && _log[2].tx[0] == 0xf2 && _log[2].tx[1] == 0x00 &&
            memcmp(_log[2].tx + 2, expected_addr, 4) == 0,
            "address write framing");
    CHECK(_log[3].len == 3 && _log[3].tx[0] == 0xf2 &&
            _log[3].tx[1] == 0x0c && _log[3].tx[2] == 0x00,
            "direction write framing");
    CHECK(_log[4].is_rx && _log[4].len == 3 + 4 && _log[4].tx[0] == 0xf3 &&
            _log[4].tx[1] == 0x08 && _log[4].tx[2] == 0x00,
            "rdata read framing");
    CHECK(data[0] == 0x11, "read data %02x", data[0]);
}

static void test_register_write_framing(void) {
    himax_dev_t dev;
    const uint8_t payload[4] = { 0xa5, 0, 0, 0 };

    ic_model_reset();
    dev_init(&dev);
    _log_count = 0;
    CHECK(himax_reg_write(&dev, HIMAX_REG_FW_CTRL, payload, 4) == 0,
            "reg write");
    CHECK(_log_count == 5, "expected 5 transactions, got %u", _log_count);
    CHECK(_log[2].len == 2 + 4 + 4, "address+data length %zu", _log[2].len);
    CHECK(_log[2].tx[0] == 0xf2 && _log[2].tx[1] == 0x00, "address cmd");
    CHECK(_log[2].tx[2] == 0x5c && _log[2].tx[3] == 0x00, "address bytes");
    CHECK(memcmp(_log[2].tx + 6, payload, 4) == 0, "payload bytes");
    CHECK(_ic.fw_ctrl[0] == 0xa5, "IC control word");
}

static void test_interface_on(void) {
    himax_dev_t dev;

    ic_model_reset();
    dev_init(&dev);
    CHECK(himax_interface_on(&dev) == 0, "interface on");

    ic_model_reset();
    dev_init(&dev);
    _ic.silent = true;
    _ic.conti = 0;
    _ic.incr4 = 0;
    CHECK(himax_interface_on(&dev) != 0, "interface on must fail");
    CHECK(strcmp(dev.stage, "interface-no-response") == 0, "stage %s",
            dev.stage);
}

static void test_chip_detect(void) {
    himax_dev_t dev;

    ic_model_reset();
    dev_init(&dev);
    CHECK(himax_chip_detect(&dev) == 0, "chip detect: %s", dev.stage);
    CHECK(dev.ic_id == HIMAX_IC_ID, "ic id %06x", dev.ic_id);
    CHECK(strstr(_ic.seq, "psw") != NULL, "safe-mode password missing");

    ic_model_reset();
    dev_init(&dev);
    _ic.ic_id_word = 0x83102a << 8;
    CHECK(himax_chip_detect(&dev) != 0, "wrong IC accepted");
    CHECK(strcmp(dev.stage, "detect-wrong-ic") == 0, "stage %s", dev.stage);
}

static void test_probe_sequence(void) {
    himax_dev_t dev;

    ic_model_reset();
    dev_init(&dev);
    CHECK(himax_probe(&dev) == 0, "probe: %s", dev.stage);
    CHECK(dev.ic_id == HIMAX_IC_ID, "probe ic id");
    CHECK(_ic.dsram_flash_reload == HIMAX_FLASH_RELOAD_HAS_FLASH,
            "flash reload policy %08x", _ic.dsram_flash_reload);
    CHECK(_ic.dsram_raw_sel == HIMAX_RAW_OUT_SELECT, "raw out select %08x",
            _ic.dsram_raw_sel);
    CHECK(strstr(_ic.seq, "rst-assert,rst-release") != NULL,
            "pin reset missing: %s", _ic.seq);
    CHECK(strstr(_ic.seq, "psw") != NULL, "safe mode not entered");
    CHECK(dev.stage[0] != 0, "stage set");
}

static void test_probe_reload_timeout(void) {
    himax_dev_t dev;

    /* A stale reload word must be cleared, and a reload that never completes
     * must stop the probe with a stage the daemon can log. */
    ic_model_reset();
    dev_init(&dev);
    _ic.dsram_reload = 0xdeadbeef;
    _ic.never_reload = true;
    CHECK(himax_probe(&dev) != 0, "probe must fail without reload");
    CHECK(strcmp(dev.stage, "init-fw-reload-timeout") == 0, "stage %s",
            dev.stage);
}

static void test_raw_init_skip_reload(void) {
    himax_dev_t dev;

    /* Flash-policy board: reload word stays at 0, but the skip variant must
     * still finish raw bring-up and select raw output. */
    ic_model_reset();
    dev_init(&dev);
    _ic.never_reload = true;
    _ic.dsram_reload = 0;
    CHECK(himax_power_on_init_skip_reload(&dev, true) == 0,
            "skip-reload init must succeed");
    CHECK(strcmp(dev.stage, "init-ready") == 0, "stage %s", dev.stage);
    CHECK(_ic.dsram_raw_sel != 0, "raw output must be selected");
    CHECK(_ic.fw_started, "sense on must have run");
}

static void test_frame_read(void) {
    himax_dev_t dev;
    static uint8_t stream[HIMAX_STREAM_BYTES];

    ic_model_reset();
    dev_init(&dev);
    _log_count = 0;
    CHECK(himax_read_frame(&dev, stream) == 0, "frame read");
    CHECK(_log_count == 1, "one transaction, got %u", _log_count);
    CHECK(_log[0].len == HIMAX_BUS_R_HLEN + HIMAX_STREAM_BYTES,
            "frame read length %zu", _log[0].len);
    CHECK(_log[0].is_rx, "frame read must request data");
    CHECK(_log[0].tx[1] == HIMAX_AHB_ADDR_EVENT_STACK, "event stack cmd");
    CHECK(stream[0] == 0x00 && stream[1] == 0x01 && stream[2] == 0x02,
            "payload offset: %02x %02x %02x", stream[0], stream[1], stream[2]);
    CHECK(stream[HIMAX_STREAM_HEADER_BYTES] == 0x04, "grid offset");
    printf("  frame stream %u bytes, grid at +%u\n",
            (unsigned)HIMAX_STREAM_BYTES, HIMAX_STREAM_HEADER_BYTES);
}

static void test_bus_retry_and_error(void) {
    himax_dev_t dev;
    uint8_t data[4];

    ic_model_reset();
    dev_init(&dev);
    _log_count = 0;
    _ic.fail_at = 5; /* fourth register-access transfer is the data read */
    CHECK(himax_reg_read(&dev, HIMAX_REG_FW_CTRL, data, 4) == 0,
            "retry must recover");
    CHECK(dev.bus_errors == 0, "bus errors counted only after all retries");

    ic_model_reset();
    dev_init(&dev);
    dev.bus.xfer = NULL;
    CHECK(himax_reg_read(&dev, HIMAX_REG_FW_CTRL, data, 4) != 0,
            "no bus must fail");
}

static void test_frame_validation(void) {
    static uint8_t stream[HIMAX_STREAM_BYTES];
    uint16_t* grid = (uint16_t*)(stream + HIMAX_STREAM_HEADER_BYTES);

    /* A record with the marker and a clean baseline grid is accepted. */
    memset(stream, 0, sizeof(stream));
    stream[0] = 0x5a;
    stream[1] = 0xa5;
    for (unsigned i = 0; i < HIMAX_PIXELS; i++)
        grid[i] = 0x7ffe;
    CHECK(himax_frame_valid(stream), "clean frame rejected");

    /* A real touch only changes a few pixels and stays valid. */
    for (int r = 18; r < 23; r++)
        for (int c = 28; c < 33; c++)
            grid[r * HIMAX_COLS + c] = 0x8500;
    CHECK(himax_frame_valid(stream), "touched frame rejected");

    /* Missing marker: a misaligned stream slice. */
    stream[0] = 0x55;
    CHECK(!himax_frame_valid(stream), "frame without marker accepted");
    stream[0] = 0x5a;

    /* Byte-shifted payload: most samples fall outside the baseline window. */
    for (unsigned i = 0; i < HIMAX_PIXELS; i++)
        grid[i] = (uint16_t)((grid[i] >> 8) | (grid[i] << 8));
    CHECK(!himax_frame_valid(stream), "byte-swapped frame accepted");

    /* All-zero record (no frame published yet). */
    memset(stream, 0, sizeof(stream));
    CHECK(!himax_frame_valid(stream), "empty frame accepted");
}

int main(void) {
    test_register_read_framing();
    test_register_write_framing();
    test_interface_on();
    test_chip_detect();
    test_probe_sequence();
    test_probe_reload_timeout();
    test_raw_init_skip_reload();
    test_frame_read();
    test_bus_retry_and_error();
    test_frame_validation();
    if (_failures) {
        printf("%d failure(s)\n", _failures);
        return 1;
    }
    printf("himax_protocol_test: all checks passed\n");
    return 0;
}
