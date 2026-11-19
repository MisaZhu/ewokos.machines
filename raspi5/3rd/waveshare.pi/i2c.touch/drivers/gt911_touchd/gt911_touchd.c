/*
 * gt911_touchd.c - Goodix GT911 I2C touch daemon (raspi5, BCM2712/RP1).
 *
 * Events are emitted on the goodixd multi-touch stream contract (see
 * xtouch.c): a {0x8000, count, 0} frame header followed by one
 * {0x8000 | act<<8 | id, x, y} record per contact. xtouch maps the raw
 * panel coordinates and xserverd routes gestures / lone-finger mouse
 * compatibility, so this driver only reports contact edges.
 */
#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/i2c.h>
#include "gt911/gt911.h"

#define TP_POLL_MIN_US       8000u   /* ~125Hz while touching */
#define TP_POLL_MAX_US       20000u  /* back off to 50Hz when idle (was 20Hz:
                                        the idle path is only a cheap IRQ-line
                                        GPIO poll, but a 50ms cap added up to
                                        50ms latency to the first touch after
                                        the panel idled) */
#define TP_POLL_HOLD_US      2000u   /* wait for the next IRQ while finger stays down */
#define TP_RELEASE_GRACE_MS  40     /* absent-contact grace before RELEASE */
#define TP_RELEASE_TIMEOUT_MS 80    /* safety net for a missed final frame */
#define TP_I2C_FAIL_MAX      20     /* consecutive failures before reinit */
#define GT911_PROFILE_LEGACY 0
#define GT911_PROFILE_DPI    1

/* goodixd multi-touch stream protocol (see xtouch.c / xevent.h) */
#define TP_TOUCH_CACHE_SIZE 64u
#define TOUCH_MT_FRAME      0x8000u
#define TOUCH_ACT_PRESS     1u
#define TOUCH_ACT_MOVE      2u
#define TOUCH_ACT_RELEASE   3u
#define MT_MAX_CONTACTS     10u

typedef struct {
    bool active;
    uint16_t x, y;
    uint32_t absent_ms;   /* ms the contact has been missing from frames */
} mt_slot_t;

static	TouchCordinate_t cordinate[GOODIX_MAX_POINTS];
static 	uint8_t  number_of_cordinate = 0;
static	uint32_t poll_sleep_us = TP_POLL_MIN_US;
static	uint32_t i2c_fail_count = 0;

static uint16_t touch_data[TP_TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;
static mt_slot_t mt_slots[MT_MAX_CONTACTS];
static uint64_t mt_last_frame_ms = 0;
static bool mt_zero_frame = false;

static const GT911_Platform_t gt911_platform = {
    .bus = BCM2712_I2C_BUS_HEADER,
    .sda = -1,
    .scl = -1,
    .rst = 17,
    .irq = 4,
    .addr = 0,
};

static const GT911_Platform_t gt911_platform_dpi = {
    .bus = -1,
    .sda = 10,
    .scl = 11,
    .rst = -1,
    .irq = 27,
    .addr = 0,
};

static const GT911_Platform_t* const gt911_platform_profiles[] = {
    &gt911_platform,
    &gt911_platform_dpi,
};

static const GT911_Platform_t* gt911_platform_active =
    &gt911_platform_dpi;

static bool tp_irq_asserted(void) {
    if (gt911_platform_active->irq < 0)
        return true;
    return !bcm2712_gpio_read((uint32_t)gt911_platform_active->irq);
}

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    if (touch_data_read == touch_data_write)
        return VFS_ERR_RETRY;
    if (size < 6)
        return -1;

    memcpy(buf, touch_data[touch_data_read], 6);
    touch_data_read = (touch_data_read + 1) % TP_TOUCH_CACHE_SIZE;
    return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    return (touch_data_read != touch_data_write) ? VFS_EVT_RD : 0;
}

static bool mt_any_active(void) {
    for (uint32_t i = 0; i < MT_MAX_CONTACTS; i++) {
        if (mt_slots[i].active)
            return true;
    }
    return false;
}

/* Diff one controller frame against the last emitted contact set and queue
 * the resulting PRESS/MOVE/RELEASE records. A contact missing from frames
 * is released only after TP_RELEASE_GRACE_MS so a controller that drops a
 * finger for one or two frames does not split one gesture into tap+press.
 * A frame is committed to the queue atomically: when the ring cannot hold
 * a whole frame, drop it instead of publishing a torn one. Frames carrying
 * a due RELEASE evict stale records instead of being dropped, so a lift
 * edge can never be lost to queue pressure. */
static bool mt_emit_frame(const TouchCordinate_t* pts, uint32_t nr) {
    uint16_t frame[1 + MT_MAX_CONTACTS][3];
    uint32_t count = 0;
    uint64_t now_ms = kernel_tic_ms(0);
    bool seen[MT_MAX_CONTACTS];
    bool has_release = false;

    memset(seen, 0, sizeof(seen));
    if (nr > MT_MAX_CONTACTS)
        nr = MT_MAX_CONTACTS;

    for (uint32_t i = 0; i < nr; i++) {
        uint8_t id = pts[i].id;
        if (id >= MT_MAX_CONTACTS)
            continue;
        seen[id] = true;

        mt_slot_t* s = &mt_slots[id];
        uint16_t act;
        if (!s->active) {
            act = TOUCH_ACT_PRESS;
        }
        else if (s->x != pts[i].x || s->y != pts[i].y) {
            act = TOUCH_ACT_MOVE;
        }
        else {
            s->absent_ms = 0;
            continue;   /* stationary contact: no edge to report */
        }
        s->active = true;
        s->x = pts[i].x;
        s->y = pts[i].y;
        s->absent_ms = 0;
        frame[count][0] = (uint16_t)(TOUCH_MT_FRAME | (act << 8) | id);
        frame[count][1] = pts[i].x;
        frame[count][2] = pts[i].y;
        count++;
    }

    for (uint32_t id = 0; id < MT_MAX_CONTACTS; id++) {
        mt_slot_t* s = &mt_slots[id];
        if (!s->active || seen[id])
            continue;
        s->absent_ms += (uint32_t)(now_ms - mt_last_frame_ms);
        if (s->absent_ms < TP_RELEASE_GRACE_MS)
            continue;
        s->active = false;
        s->absent_ms = 0;
        frame[count][0] = (uint16_t)(TOUCH_MT_FRAME |
                (TOUCH_ACT_RELEASE << 8) | id);
        frame[count][1] = s->x;
        frame[count][2] = s->y;
        count++;
        has_release = true;
    }
    mt_last_frame_ms = now_ms;

    if (count == 0)
        return false;

    uint32_t used = (touch_data_write - touch_data_read) % TP_TOUCH_CACHE_SIZE;
    if (used + count + 1 > TP_TOUCH_CACHE_SIZE) {
        if (!has_release)
            return false;   /* motion only: safe to drop, slots stay active */
        /* evict oldest records until the whole frame fits */
        while ((touch_data_write - touch_data_read) % TP_TOUCH_CACHE_SIZE +
                count + 1 > TP_TOUCH_CACHE_SIZE)
            touch_data_read = (touch_data_read + 1) % TP_TOUCH_CACHE_SIZE;
    }

    touch_data[touch_data_write][0] = TOUCH_MT_FRAME;
    touch_data[touch_data_write][1] = (uint16_t)count;
    touch_data[touch_data_write][2] = 0;
    touch_data_write = (touch_data_write + 1) % TP_TOUCH_CACHE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(touch_data[touch_data_write], frame[i], sizeof(frame[i]));
        touch_data_write = (touch_data_write + 1) % TP_TOUCH_CACHE_SIZE;
    }
    return true;
}

/* link lost: every held contact is gone, emit RELEASE for all slots so
 * xserverd never keeps a ghost finger (and its synthesized mouse button)
 * down across a re-init */
static bool mt_release_all(void) {
    uint16_t frame[MT_MAX_CONTACTS][3];
    uint32_t count = 0;

    for (uint32_t id = 0; id < MT_MAX_CONTACTS; id++) {
        mt_slot_t* s = &mt_slots[id];
        if (!s->active)
            continue;
        s->active = false;
        s->absent_ms = 0;
        frame[count][0] = (uint16_t)(TOUCH_MT_FRAME |
                (TOUCH_ACT_RELEASE << 8) | id);
        frame[count][1] = s->x;
        frame[count][2] = s->y;
        count++;
    }
    if (count == 0)
        return false;

    while ((touch_data_write - touch_data_read) % TP_TOUCH_CACHE_SIZE +
            count + 1 > TP_TOUCH_CACHE_SIZE)
        touch_data_read = (touch_data_read + 1) % TP_TOUCH_CACHE_SIZE;

    touch_data[touch_data_write][0] = TOUCH_MT_FRAME;
    touch_data[touch_data_write][1] = (uint16_t)count;
    touch_data[touch_data_write][2] = 0;
    touch_data_write = (touch_data_write + 1) % TP_TOUCH_CACHE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(touch_data[touch_data_write], frame[i], sizeof(frame[i]));
        touch_data_write = (touch_data_write + 1) % TP_TOUCH_CACHE_SIZE;
    }
    return true;
}

static int tp_loop(vdevice_t* dev, void* p) {
    (void)p;
    bool need_wakeup = false;
    GT911_Status_t ret = GT911_OK;
    uint64_t now_ms = kernel_tic_ms(0);
    bool irq_asserted = tp_irq_asserted();

    /*
     * On the DPI panel the controller exposes IRQ on GPIO27. When the line is
     * inactive there is no pending touch data, so skip the expensive bit-bang
     * I2C transaction and only keep a cheap GPIO poll while idle.
     *
     * Do not treat an inactive IRQ as a release while a finger is already down:
     * GT911 only pulses/asserts IRQ when fresh data is available, so a steady
     * long-press can leave IRQ inactive until the next state change.
     */
    if (!irq_asserted) {
        /* no fresh data pending: keep advancing the release grace for
         * contacts missing from the last frame (explicit zero-contact
         * frame, or a missed final frame past the idle window) */
        if (mt_any_active() && (mt_zero_frame ||
                (now_ms - mt_last_frame_ms) >= TP_RELEASE_TIMEOUT_MS)) {
            if (mt_emit_frame(NULL, 0)) {
                poll_sleep_us = TP_POLL_MIN_US;
                vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
            }
            mt_zero_frame = true;
            mt_last_frame_ms = now_ms;
            usleep(TP_POLL_HOLD_US);
            return 0;
        }

        if (mt_any_active()) {
            usleep(TP_POLL_HOLD_US);
            return 0;
        }

        if (poll_sleep_us < TP_POLL_MAX_US) {
            poll_sleep_us <<= 1;
            if (poll_sleep_us > TP_POLL_MAX_US)
                poll_sleep_us = TP_POLL_MAX_US;
        }
        usleep(poll_sleep_us);
        return 0;
    }

    number_of_cordinate = 0;
    ret = GT911_ReadTouch(cordinate, &number_of_cordinate);

    if (ret != GT911_OK) {
        i2c_fail_count++;
        if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
            i2c_fail_count = 0;
            if (mt_release_all())
                need_wakeup = true;
            if (GT911_Init(gt911_platform_active) == GT911_OK) {
                memset(mt_slots, 0, sizeof(mt_slots));
                mt_zero_frame = false;
                mt_last_frame_ms = now_ms;
            }
        }
    } else {
        i2c_fail_count = 0;
        /* full contact set (possibly empty): diff against held slots */
        if (mt_emit_frame(cordinate, number_of_cordinate))
            need_wakeup = true;
        mt_zero_frame = (number_of_cordinate == 0);
        mt_last_frame_ms = now_ms;
    }

    if (need_wakeup) {
        poll_sleep_us = TP_POLL_MIN_US;
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    } else if (mt_any_active()) {
        poll_sleep_us = TP_POLL_MIN_US;   /* stay fast while fingers down */
    } else if (poll_sleep_us < TP_POLL_MAX_US) {
        poll_sleep_us <<= 1;
        if (poll_sleep_us > TP_POLL_MAX_US)
            poll_sleep_us = TP_POLL_MAX_US;
    }

    usleep(poll_sleep_us);
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = "/dev/touch0";
    int c = 0;

    while ((c = getopt(argc, argv, "t:")) != -1) {
        switch (c) {
        case 't': {
            int profile = atoi(optarg);
            if (profile != GT911_PROFILE_LEGACY &&
                    profile != GT911_PROFILE_DPI) {
                return -1;
            }
            gt911_platform_active = gt911_platform_profiles[profile];
            break;
        }
        default:
            return -1;
        }
    }

    if (optind < argc)
        mnt_point = argv[optind];

    GT911_Init(gt911_platform_active);
    if (gt911_platform_active->irq >= 0) {
        bcm2712_gpio_init();
        bcm2712_gpio_config((uint32_t)gt911_platform_active->irq, GPIO_FUNC_INPUT);
        bcm2712_gpio_pull((uint32_t)gt911_platform_active->irq, GPIO_PULL_UP);
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "gt911");
    dev.read = tp_read;
    dev.loop_step = tp_loop;
    dev.check_poll_events = tp_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
