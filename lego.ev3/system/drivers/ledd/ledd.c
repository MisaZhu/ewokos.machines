/*
 * ledd - EV3 status LED driver.
 *
 * Exposes the four front-panel LEDs (left/right, green/red) as a small
 * vdevice using the fixed-width led_state_t protocol from
 * arch/ev3/led_dev.h:
 *
 *   write(fd, &led_state_t, sizeof)  / dev_cntl(LED_CNTL_SET)  -> apply
 *   read(fd,  &led_state_t, sizeof)  / dev_cntl(LED_CNTL_GET)  -> current
 *
 * A text interface (dev.cmd) is kept for the shell:
 *   on|off <left|right|all> [green|red|both] | blink <ms> | pattern <id> | status
 *
 * The GPIOs are active high (da850-lego-ev3.dts "leds"); the polarity is
 * handled in arch/ev3/led.c, this driver only tracks logical state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>
#include <ewoksys/kernel_tic.h>

#include <arch/ev3/gpio.h>
#include <arch/ev3/led.h>
#include <arch/ev3/led_dev.h>

#define PULSE_HALF_MS   500
#define PATTERN_STEP_MS 150

/* Requested state (what the client asked for). */
static led_state_t _req;
/* What is physically lit right now. */
static int32_t _lit[EV3_LED_SIDE_COUNT][EV3_LED_COLOR_COUNT];

static uint64_t _anim_last = 0;
static int32_t  _anim_step = 0;
static int32_t  _blink_on = 1;

static void apply_lit(void) {
    for (int s = 0; s < EV3_LED_SIDE_COUNT; s++)
        for (int c = 0; c < EV3_LED_COLOR_COUNT; c++)
            ev3_led_set(s, c, _lit[s][c]);
}

static void lit_all(int32_t on) {
    for (int s = 0; s < EV3_LED_SIDE_COUNT; s++)
        for (int c = 0; c < EV3_LED_COLOR_COUNT; c++)
            _lit[s][c] = on;
}

static void lit_from_req(int32_t on) {
    _lit[EV3_LED_LEFT][EV3_LED_GREEN]  = on && _req.left_green  ? 1 : 0;
    _lit[EV3_LED_LEFT][EV3_LED_RED]    = on && _req.left_red    ? 1 : 0;
    _lit[EV3_LED_RIGHT][EV3_LED_GREEN] = on && _req.right_green ? 1 : 0;
    _lit[EV3_LED_RIGHT][EV3_LED_RED]   = on && _req.right_red   ? 1 : 0;
}

/* Apply a new requested state and restart animation timing. */
static void set_state(const led_state_t* st) {
    _req = *st;
    _req.left_green  = _req.left_green  ? 1 : 0;
    _req.left_red    = _req.left_red    ? 1 : 0;
    _req.right_green = _req.right_green ? 1 : 0;
    _req.right_red   = _req.right_red   ? 1 : 0;
    if (_req.blink_ms < 0)
        _req.blink_ms = 0;
    if (_req.blink_ms > 0 && _req.blink_ms < 20)
        _req.blink_ms = 20;
    if (_req.pattern < LED_PATTERN_NONE || _req.pattern > LED_PATTERN_CYCLE)
        _req.pattern = LED_PATTERN_NONE;

    _anim_last = kernel_tic_ms(0);
    _anim_step = 0;
    _blink_on = 1;

    switch (_req.pattern) {
    case LED_PATTERN_NONE:
        lit_from_req(1);
        break;
    case LED_PATTERN_ORANGE:
        lit_all(1);
        break;
    default:
        lit_all(0);
        break;
    }
    apply_lit();
}

static void get_state(led_state_t* st) {
    *st = _req;
    if (_req.pattern != LED_PATTERN_NONE) {
        /* report what is actually lit for patterns */
        st->left_green  = _lit[EV3_LED_LEFT][EV3_LED_GREEN];
        st->left_red    = _lit[EV3_LED_LEFT][EV3_LED_RED];
        st->right_green = _lit[EV3_LED_RIGHT][EV3_LED_GREEN];
        st->right_red   = _lit[EV3_LED_RIGHT][EV3_LED_RED];
    }
}

/* ---------------- vdevice callbacks ---------------- */

static int led_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    led_state_t st;
    get_state(&st);
    if (size > (int)sizeof(st))
        size = sizeof(st);
    memcpy(buf, &st, size);
    return size;
}

static int led_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(led_state_t))
        return -1;
    set_state((const led_state_t*)buf);
    return sizeof(led_state_t);
}

static int led_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    led_state_t st;
    switch (cmd) {
    case LED_CNTL_SET:
        memset(&st, 0, sizeof(st));
        if (proto_read_to(in, &st, sizeof(st)) != (int32_t)sizeof(st)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        set_state(&st);
        PF->clear(ret)->addi(ret, 0);
        return 0;
    case LED_CNTL_GET:
        get_state(&st);
        PF->clear(ret)->addi(ret, 0)->add(ret, &st, sizeof(st));
        return 0;
    default:
        PF->clear(ret)->addi(ret, -1);
        return -1;
    }
}

/* ---------------- text interface (shell debugging) ---------------- */

static int parse_side(const char* s) {
    if (!s) return -1;
    if (strcmp(s, "left")  == 0 || strcmp(s, "l") == 0) return EV3_LED_LEFT;
    if (strcmp(s, "right") == 0 || strcmp(s, "r") == 0) return EV3_LED_RIGHT;
    if (strcmp(s, "all")   == 0 || strcmp(s, "both") == 0) return -2;
    return -1;
}

static int parse_color(const char* s) {
    if (!s) return -1;
    if (strcmp(s, "green") == 0 || strcmp(s, "g") == 0) return EV3_LED_GREEN;
    if (strcmp(s, "red")   == 0 || strcmp(s, "r") == 0) return EV3_LED_RED;
    if (strcmp(s, "both")  == 0 || strcmp(s, "all") == 0) return -2;
    return -1;
}

static int32_t* req_field(led_state_t* st, int side, int color) {
    if (side == EV3_LED_LEFT)
        return color == EV3_LED_GREEN ? &st->left_green : &st->left_red;
    return color == EV3_LED_GREEN ? &st->right_green : &st->right_red;
}

static char* led_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(256);
    if (!buf) return NULL;

    const char* usage =
        "usage: on|off <left|right|all> [green|red|both] | "
        "blink <ms> | pattern <id> | status\n";

    if (argc <= 0) {
        snprintf(buf, 256, "%s", usage);
        return buf;
    }

    const char* op = argv[0];
    led_state_t st;
    get_state(&st);

    if (strcmp(op, "on") == 0 || strcmp(op, "off") == 0) {
        int on = (strcmp(op, "on") == 0);
        int side  = (argc > 1) ? parse_side(argv[1])  : -2;
        int color = (argc > 2) ? parse_color(argv[2]) : -2;
        if (side == -1) side = -2;
        if (color == -1) color = -2;
        st = _req;
        st.pattern = LED_PATTERN_NONE;
        st.blink_ms = 0;
        for (int s = 0; s < EV3_LED_SIDE_COUNT; s++) {
            if (side != -2 && side != s) continue;
            for (int c = 0; c < EV3_LED_COLOR_COUNT; c++) {
                if (color != -2 && color != c) continue;
                *req_field(&st, s, c) = on;
            }
        }
        set_state(&st);
        snprintf(buf, 256, "led %s side=%s color=%s\n", op,
                side == -2 ? "all" : argv[1],
                color == -2 ? "both" : argv[2]);
    } else if (strcmp(op, "blink") == 0) {
        int period = (argc > 1) ? atoi(argv[1]) : 500;
        st = _req;
        st.pattern = LED_PATTERN_NONE;
        st.blink_ms = period / 2;
        if (!st.left_green && !st.left_red && !st.right_green && !st.right_red)
            st.left_green = st.right_green = 1;
        set_state(&st);
        snprintf(buf, 256, "blink half=%d ms\n", _req.blink_ms);
    } else if (strcmp(op, "pattern") == 0) {
        memset(&st, 0, sizeof(st));
        st.pattern = (argc > 1) ? atoi(argv[1]) : LED_PATTERN_GREEN_PULSE;
        set_state(&st);
        snprintf(buf, 256, "pattern=%d\n", _req.pattern);
    } else if (strcmp(op, "status") == 0) {
        snprintf(buf, 256, "LG=%d LR=%d RG=%d RR=%d blink=%d pattern=%d\n",
                st.left_green, st.left_red, st.right_green, st.right_red,
                st.blink_ms, st.pattern);
    } else {
        snprintf(buf, 256, "%s", usage);
    }
    return buf;
}

/* ---------------- animation ---------------- */

static void anim_step(uint64_t now) {
    switch (_req.pattern) {
    case LED_PATTERN_NONE:
        if (_req.blink_ms > 0 && now - _anim_last >= (uint64_t)_req.blink_ms) {
            _anim_last = now;
            _blink_on = !_blink_on;
            lit_from_req(_blink_on);
            apply_lit();
        }
        break;

    case LED_PATTERN_GREEN_PULSE:
    case LED_PATTERN_RED_PULSE:
        if (now - _anim_last >= PULSE_HALF_MS) {
            _anim_last = now;
            _blink_on = !_blink_on;
            int c = (_req.pattern == LED_PATTERN_GREEN_PULSE) ?
                    EV3_LED_GREEN : EV3_LED_RED;
            lit_all(0);
            _lit[EV3_LED_LEFT][c] = _blink_on;
            _lit[EV3_LED_RIGHT][c] = _blink_on;
            apply_lit();
        }
        break;

    case LED_PATTERN_ORANGE:
        break;                          /* steady, set in set_state */

    case LED_PATTERN_ALTERNATE:
        if (now - _anim_last >= PATTERN_STEP_MS * 2) {
            _anim_last = now;
            _anim_step ^= 1;
            lit_all(0);
            _lit[_anim_step ? EV3_LED_RIGHT : EV3_LED_LEFT][EV3_LED_GREEN] = 1;
            apply_lit();
        }
        break;

    case LED_PATTERN_CYCLE:
        if (now - _anim_last >= PATTERN_STEP_MS * 3) {
            _anim_last = now;
            _anim_step = (_anim_step + 1) % 3;
            /* green -> orange -> red on both sides */
            int g = (_anim_step != 2);
            int r = (_anim_step != 0);
            for (int s = 0; s < EV3_LED_SIDE_COUNT; s++) {
                _lit[s][EV3_LED_GREEN] = g;
                _lit[s][EV3_LED_RED] = r;
            }
            apply_lit();
        }
        break;

    default:
        break;
    }
}

static int led_loop(vdevice_t* dev, void* p) {
    (void)dev; (void)p;

    anim_step(kernel_tic_ms(0));
    usleep(20000);   /* ~50 Hz is plenty for LED animation */
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = "/dev/led";
    if (argc > 1)
        mnt_point = argv[argc - 1];

    ev3_gpio_init();
    ev3_led_init();

    memset(&_req, 0, sizeof(_req));
    memset(_lit, 0, sizeof(_lit));
    apply_lit();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "ledd");
    dev.read = led_read;
    dev.write = led_write;
    dev.dev_cntl = led_dev_cntl;
    dev.cmd = led_cmd;
    dev.loop_step = led_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
