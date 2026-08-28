#include <ewoksys/vdevice.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include "gt911/gt911.h"

#define TP_POLL_MIN_US       8000u   /* ~125Hz while touching */
#define TP_POLL_MAX_US       50000u  /* back off to 20Hz when idle */
#define TP_RELEASE_DELAY_MS  20
#define TP_INIT_RETRY_MS     1000
#define TP_I2C_FAIL_MAX      20     /* consecutive failures before reinit */
#define GT911_PROFILE_AUTO    -1
#define GT911_PROFILE_LEGACY   0
#define GT911_PROFILE_DSI      1
#define GT911_PROFILE_ALT_I2C  2

static bool press = false;
static	TouchCordinate_t cordinate[5];
static 	uint8_t  number_of_cordinate = 0;
static 	uint64_t last_ts = 0;
static	uint16_t touch_data[3];
static	bool     has_data = false;
static  bool     tp_ready = false;
static	uint32_t poll_sleep_us = TP_POLL_MIN_US;
static	uint32_t i2c_fail_count = 0;
static  int32_t  gt911_profile = GT911_PROFILE_AUTO;
static  uint64_t tp_retry_ts = 0;
static  uint32_t tp_retry_count = 0;

static const GT911_Platform_t gt911_platform_profiles[] = {
    {
        .sda = 2,
        .scl = 3,
        .rst = 17,
        .irq = 4,
        .addr = 0,
    },
    {
        .sda = 0,
        .scl = 1,
        .rst = -1,
        .irq = -1,
        .addr = GOODIX_ADDRESS_14,
    },
    {
        .sda = 10,
        .scl = 11,
        .rst = -1,
        .irq = -1,
        .addr = 0,
    },
};

static const GT911_Platform_t* gt911_platform_active = NULL;

static void tp_log_profile(const char* tag, const GT911_Platform_t* platform, int32_t profile) {
    if (platform == NULL)
        return;

    slog("gt911_touchd: %s profile=%d sda=%d scl=%d rst=%d irq=%d addr=0x%02x\n",
            tag,
            profile,
            platform->sda,
            platform->scl,
            platform->rst,
            platform->irq,
            platform->addr);
}

static GT911_Status_t tp_init_selected(void) {
    if (gt911_profile == GT911_PROFILE_AUTO) {
        if (gt911_platform_active != NULL) {
            tp_log_profile("retry", gt911_platform_active,
                    (int32_t)(gt911_platform_active - gt911_platform_profiles));
            GT911_Status_t ret = GT911_Init(gt911_platform_active);
            if (ret == GT911_OK) {
                tp_log_profile("ready", gt911_platform_active,
                        (int32_t)(gt911_platform_active - gt911_platform_profiles));
                return ret;
            }
        }

        for (uint32_t i = 0; i < sizeof(gt911_platform_profiles) / sizeof(gt911_platform_profiles[0]); i++) {
            tp_log_profile("probe", gt911_platform_profiles + i, (int32_t)i);
            GT911_Status_t ret = GT911_Init(gt911_platform_profiles + i);
            if (ret == GT911_OK) {
                gt911_platform_active = gt911_platform_profiles + i;
                tp_log_profile("ready", gt911_platform_active, (int32_t)i);
                return ret;
            }
        }
        slog("gt911_touchd: init_failed profile=auto\n");
        return GT911_NotResponse;
    }

    gt911_platform_active = gt911_platform_profiles + gt911_profile;
    tp_log_profile("probe", gt911_platform_active, gt911_profile);
    GT911_Status_t ret = GT911_Init(gt911_platform_active);
    if (ret == GT911_OK)
        tp_log_profile("ready", gt911_platform_active, gt911_profile);
    else
        slog("gt911_touchd: init_failed profile=%d\n", gt911_profile);
    return ret;
}

static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    if (!has_data)
        return VFS_ERR_RETRY;
    if (size < 6)
        return -1;

    memcpy(buf, touch_data, 6);
    has_data = false;
    return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    return has_data ? VFS_EVT_RD : 0;
}

static int tp_loop(vdevice_t* dev, void* p) {
    (void)p;
    bool need_wakeup = false;
    uint64_t now_ms = kernel_tic_ms(0);

    if (!tp_ready) {
        if (now_ms >= tp_retry_ts) {
            tp_retry_count++;
            slog("gt911_touchd: init_try count=%u profile=%d\n",
                    tp_retry_count, gt911_profile);
            if (tp_init_selected() == GT911_OK) {
                tp_ready = true;
                i2c_fail_count = 0;
                poll_sleep_us = TP_POLL_MIN_US;
                slog("gt911_touchd: online profile=%d\n", gt911_profile);
            }
            else {
                tp_retry_ts = now_ms + TP_INIT_RETRY_MS;
            }
        }

        usleep(TP_POLL_MAX_US);
        return 0;
    }

    number_of_cordinate = 0;
    GT911_Status_t ret = GT911_ReadTouch(cordinate, &number_of_cordinate);

    if (ret == GT911_NotResponse) {
        i2c_fail_count++;
        if (i2c_fail_count >= TP_I2C_FAIL_MAX) {
            i2c_fail_count = 0;
            tp_ready = false;
            tp_retry_ts = now_ms;
            slog("gt911_touchd: link_lost profile=%d\n", gt911_profile);
        }
    } else if (ret == GT911_OK || ret == GT911_NoData) {
        i2c_fail_count = 0;
    }

    if (ret == GT911_OK && !number_of_cordinate) {
        if (press && (now_ms - last_ts) > TP_RELEASE_DELAY_MS) {
            press = false;
            touch_data[0] = press;
            touch_data[1] = cordinate[0].x;
            touch_data[2] = cordinate[0].y;
            if (!has_data) {
                has_data = true;
                need_wakeup = true;
            }
        }
    } else if (ret == GT911_OK && number_of_cordinate) {
        last_ts = now_ms;
        press = true;
        touch_data[0] = press;
        touch_data[1] = cordinate[0].x;
        touch_data[2] = cordinate[0].y;
        if (!has_data) {
            has_data = true;
            need_wakeup = true;
        }
    }

    if (need_wakeup) {
        poll_sleep_us = TP_POLL_MIN_US;
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
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
        case 't':
            gt911_profile = atoi(optarg);
            if (gt911_profile != GT911_PROFILE_AUTO &&
                    gt911_profile != GT911_PROFILE_LEGACY &&
                    gt911_profile != GT911_PROFILE_DSI &&
                    gt911_profile != GT911_PROFILE_ALT_I2C) {
                return -1;
            }
            break;
        default:
            return -1;
        }
    }

    if (optind < argc)
        mnt_point = argv[optind];

    _mmio_base = mmio_map();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "gt911");
    dev.read = tp_read;
    dev.loop_step = tp_loop;
    dev.check_poll_events = tp_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
