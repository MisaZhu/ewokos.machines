#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/i2c.h>
#include <arch/bcm283x/gpio.h>

#define TP_POLL_MIN_US           8000u
#define TP_POLL_MAX_US          50000u
/* Safety net for a lift the controller never reports: both controllers
 * normally end a touch with a zero-contact frame, but if that frame is
 * missed (GT911 ready flag never re-raises, bus noise) the active slots
 * are released after this idle window. */
#define TP_RELEASE_TIMEOUT_MS      80
/* Release debounce: a contact that vanishes for a frame or two while the
 * finger is still down must not turn into a release+re-press pair (a
 * phantom tap downstream). A release goes out only after the contact
 * stayed absent for this whole window; one that returns inside it
 * remains pressed. Same policy as q8b goodixd. */
#define TP_RELEASE_GRACE_MS        40
#define TP_INIT_RETRY_MS         1000
#define TP_I2C_FAIL_MAX            20
#define TP_I2C_RETRY_MAX            3
#define TP_I2C_SPLIT_DELAY_US      100u
/* one MT frame can carry header + press/move + release per contact;
 * keep room for several frames so a slow reader never starves edges */
#define TP_TOUCH_CACHE_SIZE        64u

#define GOODIX_ADDR_PRIMARY      0x14
#define GOODIX_ADDR_FALLBACK     0x5d
#define FT5X06_ADDR              0x38
#define DISPLAY_MCU_ADDR         0x45

#define GOODIX_REG_COMMAND      0x8040
#define GOODIX_REG_ID           0x8140
#define GOODIX_REG_STATUS       0x814e
#define GOODIX_REG_TRACK1       0x814f
#define GOODIX_POINT_SIZE             8
#define GOODIX_READY_FLAG       0x80
#define GOODIX_MAX_POINTS             5
#define GOODIX_RESET_GPIO           17
#define GOODIX_INT_GPIO              4

#define FT5X06_REG_TD_STATUS      0x02
#define FT5X06_REG_P1_XH          0x03
#define FT5X06_REG_P2_XH          0x09
#define FT5X06_REG_P1_XL          0x04
#define FT5X06_REG_P1_YH          0x05
#define FT5X06_REG_P1_YL          0x06
#define FT5X06_REG_ID             0xa3
#define FT5X06_REG_FW_VER         0xa6
#define FT5X06_POINT_SIZE            6
#define FT5X06_MAX_POINTS            5
#define FT5X06_EVENT_DOWN         0x00
#define FT5X06_EVENT_UP           0x01
#define FT5X06_EVENT_CONTACT      0x02

/* Multi-touch frame records on the 6-byte stream (q8b goodixd protocol,
 * consumed by xtouch's absolute mode):
 *   state TOUCH_MT_FRAME ........ header: x = record count, y = 0
 *   state TOUCH_MT_FRAME | (act << 8) | id ... contact edge, act is
 *     TOUCH_ACT_PRESS/MOVE/RELEASE, id 0..9 (xserverd's X_TOUCH_MAX);
 *     x/y are raw panel coordinates (a release carries the contact's
 *     last known position).
 * Both controllers report exactly the active contacts per frame, so the
 * driver diffs against the last EMITTED contact set and synthesizes
 * press/move/release edges; a dropped frame (queue full) is re-derived
 * from that same set on the next controller frame - except a frame
 * carrying releases, which is never dropped: after the last finger
 * lifts the controller may send nothing more, so its room is made by
 * evicting the oldest queued records. */
#define TOUCH_MT_FRAME          0x8000u
#define TOUCH_ACT_PRESS              1u
#define TOUCH_ACT_MOVE               2u
#define TOUCH_ACT_RELEASE            3u
#define MT_MAX_CONTACTS              10u

#define WAVESHARE_REG_TP           0x94
#define WAVESHARE_REG_LCD          0x95
#define WAVESHARE_REG_PWM          0x96
#define WAVESHARE_REG_SIZE         0x97
#define WAVESHARE_REG_ID           0x98
#define WAVESHARE_REG_VERSION      0x99

#define WAVESHARE_GPIO_AVDD         0
#define WAVESHARE_GPIO_PANEL_RESET  1
#define WAVESHARE_GPIO_ENABLE       2
#define WAVESHARE_GPIO_IOVCC        4
#define WAVESHARE_GPIO_VCC          8
#define WAVESHARE_GPIO_TS_RESET     9

#define WAVESHARE_PWR_BASE \
    ((uint16_t)(1u << WAVESHARE_GPIO_VCC))
#define WAVESHARE_PWR_ENABLE \
    ((uint16_t)(WAVESHARE_PWR_BASE | (1u << WAVESHARE_GPIO_ENABLE)))
#define WAVESHARE_PWR_TOUCH_RAILS \
    ((uint16_t)(WAVESHARE_PWR_ENABLE | (1u << WAVESHARE_GPIO_AVDD) | \
    (1u << WAVESHARE_GPIO_IOVCC)))
#define WAVESHARE_PWR_TS_RESET \
    ((uint16_t)(WAVESHARE_PWR_TOUCH_RAILS | (1u << WAVESHARE_GPIO_TS_RESET)))
#define WAVESHARE_PWR_PANEL_RESET \
    ((uint16_t)(WAVESHARE_PWR_TS_RESET | (1u << WAVESHARE_GPIO_PANEL_RESET)))

typedef enum {
    TP_OK = 0,
    TP_ERROR = 1,
    TP_NOT_RESPONSE = 2,
    TP_NO_DATA = 3
} tp_status_t;

typedef struct {
    uint8_t id;     /* controller contact id: GT911 track id / FT5x06 touch id */
    uint16_t x;
    uint16_t y;
} tp_point_t;

/* last emitted contact set, indexed by the controller's contact id */
typedef struct {
    bool active;
    uint16_t x;
    uint16_t y;
    uint64_t absent_ms; /* first frame missing this contact, 0 = present */
} mt_slot_t;

typedef struct {
        int32_t mode;
    int32_t sda;
    int32_t scl;
    const char* name;
} dsi_i2c_bus_t;

typedef enum {
        TOUCH_KIND_NONE = 0,
        TOUCH_KIND_GOODIX = 1,
        TOUCH_KIND_FT5X06 = 2,
} touch_kind_t;

enum {
        DSI_I2C_MODE_BITBANG = 0,
        DSI_I2C_MODE_I2C0 = 1,
        DSI_I2C_MODE_I2C1 = 2,
};

static const dsi_i2c_bus_t dsi_i2c_buses[] = {
    {DSI_I2C_MODE_I2C0, 44, 45, "i2c0-dsi"},
    {DSI_I2C_MODE_I2C0, 0, 1, "i2c0-id"},
    {DSI_I2C_MODE_I2C1, 2, 3, "i2c1-header"},
    {DSI_I2C_MODE_BITBANG, 44, 45, "dsi"},
    {DSI_I2C_MODE_BITBANG, 0, 1, "id"},
    {DSI_I2C_MODE_BITBANG, 2, 3, "legacy"},
    {DSI_I2C_MODE_BITBANG, 10, 11, "alt"},
};

static tp_point_t point[GOODIX_MAX_POINTS > FT5X06_MAX_POINTS ?
    GOODIX_MAX_POINTS : FT5X06_MAX_POINTS];
static uint8_t point_nr = 0;
static uint16_t touch_data[TP_TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;
static bool tp_ready = false;
static uint32_t poll_sleep_us = TP_POLL_MIN_US;
static uint32_t i2c_fail_count = 0;
static uint64_t tp_retry_ts = 0;
static uint32_t tp_retry_count = 0;
static mt_slot_t mt_slots[MT_MAX_CONTACTS];
static uint64_t mt_last_frame_ms = 0; /* last decoded controller frame */
static bool mt_zero_frame = true;     /* that frame carried zero contacts */
static uint8_t touch_addr = GOODIX_ADDR_PRIMARY;
static touch_kind_t touch_kind = TOUCH_KIND_NONE;
static const dsi_i2c_bus_t* active_bus = NULL;
static bool active_bus_hw = false;
static bool waveshare_panel_reset_tried = false;
static uint16_t waveshare_power_state = 0;
static bool waveshare_power_state_valid = false;

/* raw bit-bang primitives from arch_bcm283x i2c.c */
extern void i2c_do_start(void);
extern void i2c_do_stop(void);
extern uint32_t i2c_do_write_byte(uint8_t data);
extern uint8_t i2c_do_read_byte(int32_t ack);

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus);
static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus);

static bool touch_has_data(void) {
    return (touch_data_write - touch_data_read) > 0;
}

static void touch_push(uint16_t state, uint16_t x, uint16_t y) {
    uint16_t* evt;

    if (touch_data_write - touch_data_read >= TP_TOUCH_CACHE_SIZE)
        touch_data_read++;

    evt = touch_data[touch_data_write % TP_TOUCH_CACHE_SIZE];
    evt[0] = state;
    evt[1] = x;
    evt[2] = y;
    touch_data_write++;
}

static bool mt_any_active(void) {
    uint32_t id;

    for (id = 0; id < MT_MAX_CONTACTS; id++) {
        if (mt_slots[id].active)
            return true;
    }
    return false;
}

/* Diff one decoded controller frame against the last emitted contact set
 * and queue the whole edge frame atomically: if the queue cannot hold
 * header + every record, nothing is pushed and the slot table stays
 * untouched, so the next controller frame re-derives the same edges. A
 * frame with no change at all (stationary fingers) emits nothing.
 * Releases are debounced through TP_RELEASE_GRACE_MS and, once due, must
 * go out even on a full queue. Returns true when a frame was queued. */
static bool mt_emit_frame(const tp_point_t* pts, uint8_t n) {
    uint16_t recs[MT_MAX_CONTACTS * 2][3];
    uint32_t count = 0;
    bool seen[MT_MAX_CONTACTS];
    bool changed = false;
    uint64_t now = kernel_tic_ms(0);
    uint32_t i;
    uint32_t id;

    memset(seen, 0, sizeof(seen));
    for (i = 0; i < n; i++) {
        uint16_t x = pts[i].x;
        uint16_t y = pts[i].y;
        uint32_t act;

        id = pts[i].id;
        if (id >= MT_MAX_CONTACTS || seen[id])
            continue;
        seen[id] = true;
        if (!mt_slots[id].active) {
            act = TOUCH_ACT_PRESS;
            changed = true;
        }
        else if (mt_slots[id].x != x || mt_slots[id].y != y) {
            act = TOUCH_ACT_MOVE;
            changed = true;
        }
        else {
            continue; /* unchanged contact carries no edge */
        }
        recs[count][0] = (uint16_t)(TOUCH_MT_FRAME | (act << 8) | id);
        recs[count][1] = x;
        recs[count][2] = y;
        count++;
    }
    for (id = 0; id < MT_MAX_CONTACTS; id++) {
        if (!mt_slots[id].active || seen[id])
            continue;
        if (mt_slots[id].absent_ms == 0) {
            mt_slots[id].absent_ms = now; /* start the grace window */
            continue;
        }
        if (now - mt_slots[id].absent_ms < TP_RELEASE_GRACE_MS)
            continue;
        recs[count][0] = (uint16_t)(TOUCH_MT_FRAME |
                (TOUCH_ACT_RELEASE << 8) | id);
        recs[count][1] = mt_slots[id].x;
        recs[count][2] = mt_slots[id].y;
        count++;
        changed = true;
        mt_slots[id].active = false;
        mt_slots[id].absent_ms = 0;
    }
    if (!changed || count == 0)
        return false;
    if ((touch_data_write - touch_data_read) + 1 + count > TP_TOUCH_CACHE_SIZE) {
        /* A release must never be lost: once the last finger lifts the
         * controller may send no further frame, so a dropped release
         * frame is never re-derived and the contact stays pressed
         * forever downstream. Make room by evicting the oldest queued
         * records - stale move edges whose positions the release frame
         * supersedes anyway. Frames without a release keep the
         * drop-and-re-derive policy. */
        bool has_release = false;
        uint32_t drop;

        for (i = 0; i < count; i++) {
            if (((recs[i][0] >> 8) & 0x3u) == TOUCH_ACT_RELEASE) {
                has_release = true;
                break;
            }
        }
        if (!has_release)
            return false;
        drop = (touch_data_write - touch_data_read) + 1 + count -
            TP_TOUCH_CACHE_SIZE;
        touch_data_read += drop;
    }
    touch_push(TOUCH_MT_FRAME, (uint16_t)count, 0);
    for (i = 0; i < count; i++)
        touch_push(recs[i][0], recs[i][1], recs[i][2]);
    /* commit the emitted set only after the frame is fully queued. A
     * slot inside its release grace window keeps active=true (and its
     * last position) so a returning contact continues instead of
     * re-pressing. */
    for (id = 0; id < MT_MAX_CONTACTS; id++) {
        if (!seen[id])
            continue;
        mt_slots[id].active = true;
        mt_slots[id].absent_ms = 0;
        for (i = 0; i < n; i++) {
            if (pts[i].id == id) {
                mt_slots[id].x = pts[i].x;
                mt_slots[id].y = pts[i].y;
                break;
            }
        }
    }
    return true;
}

/* Link lost / controller reset: every active contact's lift will never
 * be reported, so release all slots immediately (no grace) and make
 * room in the queue the same way a due release frame does. */
static bool mt_release_all(void) {
    uint16_t recs[MT_MAX_CONTACTS][3];
    uint32_t count = 0;
    uint32_t id;
    uint32_t i;

    for (id = 0; id < MT_MAX_CONTACTS; id++) {
        if (!mt_slots[id].active)
            continue;
        recs[count][0] = (uint16_t)(TOUCH_MT_FRAME |
                (TOUCH_ACT_RELEASE << 8) | id);
        recs[count][1] = mt_slots[id].x;
        recs[count][2] = mt_slots[id].y;
        count++;
        mt_slots[id].active = false;
        mt_slots[id].absent_ms = 0;
    }
    if (count == 0)
        return false;
    if ((touch_data_write - touch_data_read) + 1 + count > TP_TOUCH_CACHE_SIZE)
        touch_data_read += (touch_data_write - touch_data_read) + 1 + count -
            TP_TOUCH_CACHE_SIZE;
    touch_push(TOUCH_MT_FRAME, (uint16_t)count, 0);
    for (i = 0; i < count; i++)
        touch_push(recs[i][0], recs[i][1], recs[i][2]);
    return true;
}

static int32_t dsi_i2c_select(const dsi_i2c_bus_t* bus) {
    active_bus = bus;
    active_bus_hw = (bus->mode != DSI_I2C_MODE_BITBANG);
    if (bus->mode == DSI_I2C_MODE_I2C0) {
        if (bcm283x_i2c0_init(bus->sda, bus->scl) != 0)
            return -1;
    }
    else if (bus->mode == DSI_I2C_MODE_I2C1) {
        if (bcm283x_i2c1_init(bus->sda, bus->scl) != 0)
            return -1;
    }
    else {
        i2c_init(bus->sda, bus->scl);
    }
    usleep(2000);
    return 0;
}

static uint32_t dsi_i2c_write_reg16(uint8_t addr, uint16_t reg,
        const uint8_t* data, uint16_t len) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);
    uint16_t i;

    if (active_bus_hw) {
        uint8_t buf[258];

        if (len > sizeof(buf) - 2)
            return 1;
        buf[0] = (uint8_t)(reg >> 8);
        buf[1] = (uint8_t)(reg & 0xff);
        for (i = 0; i < len; i++)
            buf[i + 2] = data[i];
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
            return bcm283x_i2c1_write(addr, buf, len + 2) == 0 ? 0 : 1;
        return bcm283x_i2c0_write(addr, buf, len + 2) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    test |= i2c_do_write_byte((uint8_t)(reg >> 8));
    test |= i2c_do_write_byte((uint8_t)(reg & 0xff));
    for (i = 0; i < len; i++)
        test |= i2c_do_write_byte(data[i]);
    i2c_do_stop();
    return test;
}

static uint32_t dsi_i2c_read_reg16(uint8_t addr, uint16_t reg,
        uint8_t* data, uint16_t len) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);
    uint16_t i;

    if (active_bus_hw) {
        uint8_t reg_buf[2];

        reg_buf[0] = (uint8_t)(reg >> 8);
        reg_buf[1] = (uint8_t)(reg & 0xff);
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
                return bcm283x_i2c1_write_read(addr, reg_buf, sizeof(reg_buf), data, len) == 0 ? 0 : 1;
        return bcm283x_i2c0_write_read(addr, reg_buf, sizeof(reg_buf), data, len) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    test |= i2c_do_write_byte((uint8_t)(reg >> 8));
    test |= i2c_do_write_byte((uint8_t)(reg & 0xff));
    i2c_do_start();
    test |= i2c_do_write_byte(addr8 | 0x01);
    for (i = 0; i < len; i++)
        data[i] = i2c_do_read_byte(i + 1 < len);
    i2c_do_stop();
    return test;
}

static uint32_t dsi_i2c_read_reg16_split(uint8_t addr, uint16_t reg,
        uint8_t* data, uint16_t len) {
    uint8_t reg_buf[2];

    reg_buf[0] = (uint8_t)(reg >> 8);
    reg_buf[1] = (uint8_t)(reg & 0xff);

    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1) {
            if (bcm283x_i2c1_write(addr, reg_buf, sizeof(reg_buf)) != 0)
                return 1;
            usleep(TP_I2C_SPLIT_DELAY_US);
            return bcm283x_i2c1_read(addr, data, len) == 0 ? 0 : 1;
        }
        if (bcm283x_i2c0_write(addr, reg_buf, sizeof(reg_buf)) != 0)
            return 1;
        usleep(TP_I2C_SPLIT_DELAY_US);
        return bcm283x_i2c0_read(addr, data, len) == 0 ? 0 : 1;
    }

    return dsi_i2c_read_reg16(addr, reg, data, len);
}

static uint32_t dsi_i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);

    if (active_bus_hw) {
        uint8_t buf[2];

        buf[0] = reg;
        buf[1] = value;
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
            return bcm283x_i2c1_write(addr, buf, sizeof(buf)) == 0 ? 0 : 1;
        return bcm283x_i2c0_write(addr, buf, sizeof(buf)) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    test |= i2c_do_write_byte(reg);
    test |= i2c_do_write_byte(value);
    i2c_do_stop();
    return test;
}

static uint32_t dsi_i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* value) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);

    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
            return bcm283x_i2c1_write_read(addr, &reg, 1, value, 1) == 0 ? 0 : 1;
        return bcm283x_i2c0_write_read(addr, &reg, 1, value, 1) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    test |= i2c_do_write_byte(reg);
    i2c_do_start();
    test |= i2c_do_write_byte(addr8 | 0x01);
    *value = i2c_do_read_byte(0);
    i2c_do_stop();
    return test;
}

static uint32_t dsi_i2c_read_reg8_split(uint8_t addr, uint8_t reg, uint8_t* value) {
    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1) {
            if (bcm283x_i2c1_write(addr, &reg, 1) != 0)
                return 1;
            usleep(TP_I2C_SPLIT_DELAY_US);
            return bcm283x_i2c1_read(addr, value, 1) == 0 ? 0 : 1;
        }
        if (bcm283x_i2c0_write(addr, &reg, 1) != 0)
            return 1;
        usleep(TP_I2C_SPLIT_DELAY_US);
        return bcm283x_i2c0_read(addr, value, 1) == 0 ? 0 : 1;
    }

    return dsi_i2c_read_reg8(addr, reg, value);
}

static uint32_t dsi_i2c_read_regs8(uint8_t addr, uint8_t reg,
        uint8_t* data, uint16_t len) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);
    uint16_t i;

    if (len == 0)
        return 0;

    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
            return bcm283x_i2c1_write_read(addr, &reg, 1, data, len) == 0 ? 0 : 1;
        return bcm283x_i2c0_write_read(addr, &reg, 1, data, len) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    test |= i2c_do_write_byte(reg);
    i2c_do_start();
    test |= i2c_do_write_byte(addr8 | 0x01);
    for (i = 0; i < len; i++)
        data[i] = i2c_do_read_byte(i + 1 < len);
    i2c_do_stop();
    return test;
}

static uint32_t dsi_i2c_read_regs8_split(uint8_t addr, uint8_t reg,
        uint8_t* data, uint16_t len) {
    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1) {
            if (bcm283x_i2c1_write(addr, &reg, 1) != 0)
                return 1;
            usleep(TP_I2C_SPLIT_DELAY_US);
            return bcm283x_i2c1_read(addr, data, len) == 0 ? 0 : 1;
        }
        if (bcm283x_i2c0_write(addr, &reg, 1) != 0)
            return 1;
        usleep(TP_I2C_SPLIT_DELAY_US);
        return bcm283x_i2c0_read(addr, data, len) == 0 ? 0 : 1;
    }

    return dsi_i2c_read_regs8(addr, reg, data, len);
}

static uint32_t dsi_i2c_read_reg8_retry(uint8_t addr, uint8_t reg, uint8_t* value) {
    uint32_t attempt;

    for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
        if (dsi_i2c_read_reg8(addr, reg, value) == 0)
            return 0;
        if (dsi_i2c_read_reg8_split(addr, reg, value) == 0)
            return 0;
        if (!active_bus_hw || active_bus == NULL)
            break;
        usleep(2000);
        if (dsi_i2c_select(active_bus) != 0)
            break;
        usleep(2000);
    }
    return 1;
}

static uint32_t dsi_i2c_read_regs8_retry(uint8_t addr, uint8_t reg,
        uint8_t* data, uint16_t len) {
    uint32_t attempt;

    for (attempt = 0; attempt < TP_I2C_RETRY_MAX; attempt++) {
        if (dsi_i2c_read_regs8(addr, reg, data, len) == 0)
            return 0;
        if (dsi_i2c_read_regs8_split(addr, reg, data, len) == 0)
            return 0;
        if (!active_bus_hw || active_bus == NULL)
            break;
        usleep(2000);
        if (dsi_i2c_select(active_bus) != 0)
            break;
        usleep(2000);
    }
    return 1;
}

static uint32_t dsi_i2c_probe_addr_only(uint8_t addr) {
    uint32_t test = 0;
    uint8_t addr8 = (uint8_t)(addr << 1);

    if (active_bus_hw) {
        if (active_bus->mode == DSI_I2C_MODE_I2C1)
            return bcm283x_i2c1_probe(addr) == 0 ? 0 : 1;
        return bcm283x_i2c0_probe(addr) == 0 ? 0 : 1;
    }

    i2c_do_start();
    test |= i2c_do_write_byte(addr8);
    i2c_do_stop();
    return test;
}

static bool waveshare_mcu_bus(const dsi_i2c_bus_t* bus) {
    if (bus == NULL)
        return false;
    return bus->mode == DSI_I2C_MODE_I2C0 && bus->sda == 44 && bus->scl == 45;
}

static bool waveshare_mcu_present(const dsi_i2c_bus_t* bus) {
    if (!waveshare_mcu_bus(bus))
        return false;
    return dsi_i2c_probe_addr_only(DISPLAY_MCU_ADDR) == 0;
}

static int32_t waveshare_mcu_write_power(uint16_t state, uint32_t settle_ms, const char* tag) {
    (void)tag;

    if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP,
                (uint8_t)(state >> 8)) != 0)
        return -1;
    if (dsi_i2c_write_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD,
                (uint8_t)(state & 0xff)) != 0)
        return -1;
    usleep(settle_ms * 1000u);
    return 0;
}

static int32_t waveshare_mcu_sync_power(void) {
    uint8_t tp = 0;
    uint8_t lcd = 0;

    if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_TP, &tp) != 0)
        return -1;
    if (dsi_i2c_read_reg8(DISPLAY_MCU_ADDR, WAVESHARE_REG_LCD, &lcd) != 0)
        return -1;

    waveshare_power_state = (uint16_t)(((uint16_t)tp << 8) | lcd);
    waveshare_power_state_valid = true;
    return 0;
}

static int32_t waveshare_mcu_set_gpio(uint8_t gpio, bool level,
        uint32_t settle_ms, const char* tag) {
    uint16_t bit = (uint16_t)(1u << gpio);

    if (!waveshare_power_state_valid && waveshare_mcu_sync_power() != 0)
        return -1;

    if (level)
        waveshare_power_state |= bit;
    else
        waveshare_power_state &= (uint16_t)~bit;

    return waveshare_mcu_write_power(waveshare_power_state, settle_ms, tag);
}

static tp_status_t waveshare_probe_goodix(const dsi_i2c_bus_t* bus) {
    (void)bus;
    return goodix_probe_candidates(bus);
}

static tp_status_t ft5x06_probe(const dsi_i2c_bus_t* bus) {
    uint8_t chip_id = 0xff;
    uint8_t fw_ver = 0xff;

    (void)bus;

    if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_ID, &chip_id) != 0) {
        if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver) != 0)
            return TP_NOT_RESPONSE;
    }
    else {
        (void)dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_FW_VER, &fw_ver);
    }

    touch_addr = FT5X06_ADDR;
    touch_kind = TOUCH_KIND_FT5X06;
    return TP_OK;
}

static tp_status_t waveshare_mcu_probe_touch_sequence(const dsi_i2c_bus_t* bus) {
    if (!waveshare_mcu_present(bus))
        return TP_NOT_RESPONSE;

    if (waveshare_mcu_sync_power() != 0)
        return TP_NOT_RESPONSE;

    if (waveshare_probe_goodix(bus) == TP_OK)
        return TP_OK;

    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_VCC, true, 20u, "vcc") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_ENABLE, true, 20u, "enable") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_AVDD, true, 40u, "avdd") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_IOVCC, true, 60u, "iovcc") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_probe_goodix(bus) == TP_OK)
        return TP_OK;

    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u,
                "touch-assert") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_probe_goodix(bus) == TP_OK)
        return TP_OK;

    if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u,
                "touch-release") != 0)
        return TP_NOT_RESPONSE;
    if (waveshare_probe_goodix(bus) == TP_OK)
        return TP_OK;

    if (!waveshare_panel_reset_tried) {
        waveshare_panel_reset_tried = true;
        if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, true, 20u,
                    "panel-assert") != 0)
            return TP_NOT_RESPONSE;
        if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_PANEL_RESET, false, 180u,
                    "panel-release") != 0)
            return TP_NOT_RESPONSE;
        if (waveshare_probe_goodix(bus) == TP_OK)
            return TP_OK;

        if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, true, 20u,
                    "touch-cycle-assert") != 0)
            return TP_NOT_RESPONSE;
        if (waveshare_mcu_set_gpio(WAVESHARE_GPIO_TS_RESET, false, 120u,
                    "touch-cycle-release") != 0)
            return TP_NOT_RESPONSE;
        if (waveshare_probe_goodix(bus) == TP_OK)
            return TP_OK;
    }

    return TP_NOT_RESPONSE;
}

static void goodix_reset_select(const dsi_i2c_bus_t* bus, uint8_t addr) {
    int32_t int_level;

    if (bus == NULL)
        return;
    if (bus->sda != 2 || bus->scl != 3)
        return;
    if (addr != GOODIX_ADDR_PRIMARY && addr != GOODIX_ADDR_FALLBACK)
        return;

    int_level = (addr == GOODIX_ADDR_FALLBACK) ? 1 : 0;

    bcm283x_gpio_config(GOODIX_RESET_GPIO, GPIO_FUNC_OUTPUT);
    bcm283x_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_OUTPUT);
    bcm283x_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_NONE);

    if (int_level != 0)
        bcm283x_gpio_set(GOODIX_INT_GPIO);
    else
        bcm283x_gpio_clr(GOODIX_INT_GPIO);

    bcm283x_gpio_clr(GOODIX_RESET_GPIO);
    usleep(20000);
    bcm283x_gpio_set(GOODIX_RESET_GPIO);
    usleep(60000);

    bcm283x_gpio_config(GOODIX_INT_GPIO, GPIO_FUNC_INPUT);
    bcm283x_gpio_pull(GOODIX_INT_GPIO, GPIO_PULL_UP);
    usleep(20000);
}

static bool goodix_id_valid(const uint8_t* id_buf) {
    if ((id_buf[0] == 0 && id_buf[1] == 0 &&
                id_buf[2] == 0 && id_buf[3] == 0) ||
            (id_buf[0] == 0xff && id_buf[1] == 0xff &&
             id_buf[2] == 0xff && id_buf[3] == 0xff))
        return false;
    return true;
}

static tp_status_t goodix_write_reg(uint16_t reg, const uint8_t* data, uint16_t len) {
    return dsi_i2c_write_reg16(touch_addr, reg, data, len) == 0 ?
        TP_OK : TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_reg(uint16_t reg, uint8_t* data, uint16_t len) {
    if (dsi_i2c_read_reg16(touch_addr, reg, data, len) == 0)
        return TP_OK;
    if (waveshare_mcu_bus(active_bus) &&
            dsi_i2c_read_reg16_split(touch_addr, reg, data, len) == 0)
        return TP_OK;
    return TP_NOT_RESPONSE;
}

static tp_status_t goodix_set_command(uint8_t value) {
    return goodix_write_reg(GOODIX_REG_COMMAND, &value, 1);
}

static tp_status_t goodix_probe_addr(uint8_t addr, uint8_t* id_buf) {
    tp_status_t ret;

    touch_addr = addr;
        touch_kind = TOUCH_KIND_GOODIX;
    ret = goodix_read_reg(GOODIX_REG_ID, id_buf, 4);
    return ret;
}

static tp_status_t goodix_probe_candidates(const dsi_i2c_bus_t* bus) {
    static const uint8_t candidates_default[] = {
        GOODIX_ADDR_PRIMARY,
        GOODIX_ADDR_FALLBACK,
    };
    static const uint8_t candidates_waveshare[] = {
        GOODIX_ADDR_FALLBACK,
        GOODIX_ADDR_PRIMARY,
    };
    uint8_t id_buf[4];
    const uint8_t* candidates;
    uint32_t candidate_nr;
    uint32_t addr_i;

    if (waveshare_mcu_bus(bus)) {
        candidates = candidates_waveshare;
        candidate_nr = sizeof(candidates_waveshare);
    }
    else {
        candidates = candidates_default;
        candidate_nr = sizeof(candidates_default);
    }

    for (addr_i = 0; addr_i < candidate_nr; addr_i++) {
        goodix_reset_select(bus, candidates[addr_i]);
        memset(id_buf, 0, sizeof(id_buf));
        if (goodix_probe_addr(candidates[addr_i], id_buf) != TP_OK)
            continue;
        if (!goodix_id_valid(id_buf))
            continue;

        goodix_set_command(0x00);
        return TP_OK;
    }

    return TP_NOT_RESPONSE;
}

static tp_status_t goodix_init(void) {
    uint32_t bus_i;

    for (bus_i = 0; bus_i < sizeof(dsi_i2c_buses) / sizeof(dsi_i2c_buses[0]); bus_i++) {
        if (dsi_i2c_select(&dsi_i2c_buses[bus_i]) != 0) {
            continue;
        }
        usleep(20000);

                if (ft5x06_probe(&dsi_i2c_buses[bus_i]) == TP_OK)
                        return TP_OK;
                if (waveshare_mcu_probe_touch_sequence(&dsi_i2c_buses[bus_i]) == TP_OK)
            return TP_OK;
        else if (goodix_probe_candidates(&dsi_i2c_buses[bus_i]) == TP_OK)
            return TP_OK;

    }

    return TP_NOT_RESPONSE;
}

static tp_status_t goodix_read_touch(tp_point_t* pts, uint8_t* nr) {
    uint8_t status;
    /* track id leads each 8-byte record (0x814f + i*8, stable across the
     * contact's life), then x/y low-high pairs — the same layout the
     * Linux goodix driver parses; read the id byte with the record */
    uint8_t buf[GOODIX_POINT_SIZE + 1];
    uint8_t i;

    if (goodix_read_reg(GOODIX_REG_STATUS, &status, 1) != TP_OK)
        return TP_NOT_RESPONSE;
    if ((status & GOODIX_READY_FLAG) == 0)
        return TP_NO_DATA;

    *nr = status & 0x0f;
    if (*nr > GOODIX_MAX_POINTS)
        *nr = GOODIX_MAX_POINTS;

    for (i = 0; i < *nr; i++) {
        if (goodix_read_reg((uint16_t)(GOODIX_REG_TRACK1 + i * GOODIX_POINT_SIZE),
                    buf, sizeof(buf)) != TP_OK)
            return TP_NOT_RESPONSE;
        pts[i].id = (uint8_t)(buf[0] & 0x0f);
        pts[i].x = (uint16_t)((buf[2] << 8) | buf[1]);
        pts[i].y = (uint16_t)((buf[4] << 8) | buf[3]);
    }

    status = 0;
    goodix_write_reg(GOODIX_REG_STATUS, &status, 1);
    return TP_OK;
}

static tp_status_t ft5x06_read_touch(tp_point_t* pts, uint8_t* nr) {
    uint8_t buf[5];
    /* P2..Pn records when more than one finger is reported */
    uint8_t rest[FT5X06_POINT_SIZE * (FT5X06_MAX_POINTS - 1)];
    uint8_t count;
    uint8_t event;
    uint8_t i;

    *nr = 0;

    if (dsi_i2c_read_regs8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS,
                    buf, sizeof(buf)) != 0) {
        if (dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_TD_STATUS, &buf[0]) != 0 ||
                        dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XH, &buf[1]) != 0 ||
                        dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_XL, &buf[2]) != 0 ||
                        dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YH, &buf[3]) != 0 ||
                        dsi_i2c_read_reg8_retry(FT5X06_ADDR, FT5X06_REG_P1_YL, &buf[4]) != 0)
            return TP_NOT_RESPONSE;
    }

    /*
     * Garbage frame: a 0xff TD_STATUS is the data phase clocking in
     * all-ones (controller mid-boot or a NAK'd read), not a real
     * 15-touch report — drop it instead of parsing phantom points.
     */
    if (buf[0] == 0xff)
        return TP_NO_DATA;

    count = (uint8_t)(buf[0] & 0x0f);
    if (count > FT5X06_MAX_POINTS)
        count = FT5X06_MAX_POINTS;
    if (count == 0)
        return TP_OK;

    /*
     * Every reported point register must be consumed: this controller
     * latches its report frame while extra points sit unread —
     * TD_STATUS freezes at count>=2, P1 repeats forever, the release
     * never arrives and the touch service dies. Read P2..Pn in one
     * burst and parse them alongside P1.
     */
    if (count > 1) {
        if (dsi_i2c_read_regs8_retry(FT5X06_ADDR, FT5X06_REG_P2_XH,
                rest, (uint16_t)((count - 1) * FT5X06_POINT_SIZE)) != 0)
            return TP_NOT_RESPONSE;
    }

    /* P1: XH = event<<6 | x-high, YH = touch-id<<4 | y-high */
    event = (uint8_t)(buf[1] >> 6);
    if (event == FT5X06_EVENT_DOWN || event == FT5X06_EVENT_CONTACT) {
        pts[*nr].id = (uint8_t)((buf[3] >> 4) & 0x0f);
        pts[*nr].x = (uint16_t)(((uint16_t)(buf[1] & 0x0f) << 8) | buf[2]);
        pts[*nr].y = (uint16_t)(((uint16_t)(buf[3] & 0x0f) << 8) | buf[4]);
        (*nr)++;
    }
    /* P2..Pn: same 6-byte record shape. UP/reserved entries are lifts,
     * not active contacts — leaving them out of the set lets the slot
     * diff release them through the grace window. */
    for (i = 1; i < count; i++) {
        const uint8_t* p = rest + (i - 1) * FT5X06_POINT_SIZE;

        event = (uint8_t)(p[0] >> 6);
        if (event != FT5X06_EVENT_DOWN && event != FT5X06_EVENT_CONTACT)
            continue;
        pts[*nr].id = (uint8_t)((p[2] >> 4) & 0x0f);
        pts[*nr].x = (uint16_t)(((uint16_t)(p[0] & 0x0f) << 8) | p[1]);
        pts[*nr].y = (uint16_t)(((uint16_t)(p[2] & 0x0f) << 8) | p[3]);
        (*nr)++;
    }
    return TP_OK;
}

static tp_status_t tp_read_touch(tp_point_t* pts, uint8_t* nr) {
        if (touch_kind == TOUCH_KIND_FT5X06)
                return ft5x06_read_touch(pts, nr);
        return goodix_read_touch(pts, nr);
}

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    if (!touch_has_data())
        return VFS_ERR_RETRY;
    if (size < 6)
        return -1;

    memcpy(buf, touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 6);
    memset(touch_data[touch_data_read % TP_TOUCH_CACHE_SIZE], 0,
                    sizeof(touch_data[0]));
    touch_data_read++;
    return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    return touch_has_data() ? VFS_EVT_RD : 0;
}

static int tp_loop(vdevice_t* dev, void* p) {
    tp_status_t ret;
    bool need_wakeup = false;
    uint64_t now_ms = kernel_tic_ms(0);

    (void)p;

    if (!tp_ready) {
        if (now_ms >= tp_retry_ts) {
            tp_retry_count++;
            if (goodix_init() == TP_OK) {
                tp_ready = true;
                i2c_fail_count = 0;
                poll_sleep_us = TP_POLL_MIN_US;
                /* the controller may have been power-cycled by the
                 * probe: start from an empty contact set */
                memset(mt_slots, 0, sizeof(mt_slots));
                mt_last_frame_ms = now_ms;
                mt_zero_frame = true;
            }
            else {
                tp_retry_ts = now_ms + TP_INIT_RETRY_MS;
            }
        }

        usleep(TP_POLL_MAX_US);
        return 0;
    }

    point_nr = 0;
    ret = tp_read_touch(point, &point_nr);

    if (ret == TP_NOT_RESPONSE) {
        i2c_fail_count++;
        if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
            i2c_fail_count = 0;
            need_wakeup = mt_release_all() || need_wakeup;
            tp_ready = false;
            tp_retry_ts = now_ms;
            slog("dsi_touchd: link_lost bus=%s addr=0x%02x kind=%s\n",
                            active_bus == NULL ? "?" : active_bus->name,
                            touch_addr,
                            touch_kind == TOUCH_KIND_FT5X06 ? "ft5x06" : "goodix");
        }
    }
    else if (ret == TP_OK || ret == TP_NO_DATA) {
        i2c_fail_count = 0;
    }

    if (ret == TP_OK) {
        /* a decoded frame (even zero contacts) is the controller's
         * current truth: diff it against the emitted set */
        mt_last_frame_ms = now_ms;
        mt_zero_frame = (point_nr == 0);
        if (mt_emit_frame(point, point_nr))
            need_wakeup = true;
    }

    /* Advance the release grace window while contacts are still marked
     * active but no contact frame carries them: after an explicit
     * zero-contact frame feed empty diffs every tick (the controller
     * sends nothing more once the last finger lifts); after a missed
     * final frame wait out the idle window first, then do the same.
     * A no-op once no slot is active. */
    if (mt_any_active() &&
            (mt_zero_frame ||
             now_ms - mt_last_frame_ms >= TP_RELEASE_TIMEOUT_MS)) {
        if (mt_emit_frame(NULL, 0))
            need_wakeup = true;
    }

    if (need_wakeup) {
        poll_sleep_us = TP_POLL_MIN_US;
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }
    else if (mt_any_active()) {
        poll_sleep_us = TP_POLL_MIN_US;
    }
    else if (poll_sleep_us < TP_POLL_MAX_US) {
        poll_sleep_us <<= 1;
        if (poll_sleep_us > TP_POLL_MAX_US)
            poll_sleep_us = TP_POLL_MAX_US;
    }

    usleep(poll_sleep_us);
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = "/dev/touch0";
    vdevice_t dev;

    if (argc > 1)
        mnt_point = argv[1];

    _mmio_base = mmio_map();

    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "dsi_touch");
    dev.read = tp_read;
    dev.loop_step = tp_loop;
    dev.check_poll_events = tp_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
