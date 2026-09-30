/*
 * beepd - EV3 beeper (speaker) driver.
 *
 * Wraps arch/ev3/sound.c as a vdevice mounted at /dev/beep using the
 * fixed-width protocol from arch/ev3/beep_dev.h:
 *
 *   write(fd, &beep_cmd_t, sizeof) / dev_cntl(BEEP_CNTL_PLAY)  -> play/stop
 *   read(fd, &beep_state_t, sizeof) / dev_cntl(BEEP_CNTL_GET)  -> status
 *
 * Text interface (dev.cmd) for the shell:
 *   tone <hz> | beep <hz> <ms> | stop | melody <id> | volume <0-100> | status
 *
 * The one-shot beep is scheduled in loop_step: we record the deadline,
 * start the tone immediately, and cut it off when the timer expires. The
 * melody player steps through a small note table the same way.
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
#include <arch/ev3/sound.h>
#include <arch/ev3/beep_dev.h>

static int32_t  _cur_freq = 0;
static uint64_t _tone_deadline = 0;    /* 0 = continuous */

/* Simple melodies: array of (freq, ms) pairs, terminated by (0,0). */
typedef struct { int32_t freq; int32_t ms; } note_t;

static const note_t _melody_startup[] = {
    { 523, 100 }, { 659, 100 }, { 784, 100 }, { 1047, 200 }, { 0, 0 }
};
static const note_t _melody_beep_twice[] = {
    { 880, 120 }, { 0, 60 }, { 880, 120 }, { 0, 0 }
};
static const note_t _melody_alert[] = {
    { 1200, 80 }, { 900, 80 }, { 1200, 80 }, { 900, 80 }, { 0, 0 }
};

static const note_t* _melody = NULL;
static int32_t  _melody_id = 0;
static int32_t  _melody_idx = 0;
static uint64_t _melody_next = 0;

static void play_freq(int32_t freq) {
    _cur_freq = freq > 0 ? freq : 0;
    if (freq > 0)
        ev3_sound_tone(freq);
    else
        ev3_sound_stop();
}

static void stop_sound(void) {
    _tone_deadline = 0;
    _melody = NULL;
    _melody_id = 0;
    play_freq(0);
}

static void start_tone(int32_t freq) {
    _melody = NULL;
    _melody_id = 0;
    _tone_deadline = 0;
    play_freq(freq);
}

static void start_beep(int32_t freq, int32_t ms) {
    if (ms < 10) ms = 10;
    _melody = NULL;
    _melody_id = 0;
    _tone_deadline = kernel_tic_ms(0) + (uint64_t)ms;
    play_freq(freq);
}

static void start_melody(int32_t id) {
    _tone_deadline = 0;
    switch (id) {
    case 1: _melody = _melody_startup;    break;
    case 2: _melody = _melody_beep_twice; break;
    case 3: _melody = _melody_alert;      break;
    default: _melody = NULL; break;
    }
    _melody_id = _melody ? id : 0;
    _melody_idx = 0;
    _melody_next = 0;
    if (_melody) {
        play_freq(_melody[0].freq);
        _melody_next = kernel_tic_ms(0) + (uint64_t)_melody[0].ms;
    } else {
        play_freq(0);
    }
}

static int32_t do_command(const beep_cmd_t* c) {
    if (c->volume > 0)
        ev3_sound_set_volume(c->volume);

    switch (c->cmd) {
    case BEEP_CMD_TONE:
        start_tone(c->freq_hz);
        return 0;
    case BEEP_CMD_BEEP:
        start_beep(c->freq_hz, c->duration_ms);
        return 0;
    case BEEP_CMD_STOP:
        stop_sound();
        return 0;
    case BEEP_CMD_MELODY:
        start_melody(c->melody);
        return 0;
    default:
        return -1;
    }
}

static void get_state(beep_state_t* st) {
    uint64_t now = kernel_tic_ms(0);
    st->playing = (_cur_freq > 0 || _melody != NULL) ? 1 : 0;
    st->freq_hz = _cur_freq;
    st->volume  = ev3_sound_get_volume();
    st->melody  = _melody_id;
    if (_melody != NULL)
        st->remain_ms = -1;
    else if (_tone_deadline == 0)
        st->remain_ms = st->playing ? -1 : 0;
    else
        st->remain_ms = (now >= _tone_deadline) ? 0 : (int32_t)(_tone_deadline - now);
}

/* ---------------- vdevice callbacks ---------------- */

static int beep_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    beep_state_t st;
    get_state(&st);
    if (size > (int)sizeof(st))
        size = sizeof(st);
    memcpy(buf, &st, size);
    return size;
}

static int beep_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(beep_cmd_t))
        return -1;
    if (do_command((const beep_cmd_t*)buf) != 0)
        return -1;
    return sizeof(beep_cmd_t);
}

static int beep_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch (cmd) {
    case BEEP_CNTL_PLAY: {
        beep_cmd_t c;
        memset(&c, 0, sizeof(c));
        if (proto_read_to(in, &c, sizeof(c)) != (int32_t)sizeof(c)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        int32_t res = do_command(&c);
        PF->clear(ret)->addi(ret, res);
        return res;
    }
    case BEEP_CNTL_GET: {
        beep_state_t st;
        get_state(&st);
        PF->clear(ret)->addi(ret, 0)->add(ret, &st, sizeof(st));
        return 0;
    }
    default:
        PF->clear(ret)->addi(ret, -1);
        return -1;
    }
}

/* ---------------- text interface (shell debugging) ---------------- */

static char* beep_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(128);
    if (!buf) return NULL;

    const char* usage =
        "usage: tone <hz> | beep <hz> <ms> | stop | melody <id> | volume <0-100> | status\n";

    if (argc <= 0) {
        snprintf(buf, 128, "%s", usage);
        return buf;
    }

    const char* op = argv[0];
    if (strcmp(op, "tone") == 0) {
        int32_t hz = (argc > 1) ? atoi(argv[1]) : 440;
        start_tone(hz);
        snprintf(buf, 128, "tone %d Hz\n", hz);
    } else if (strcmp(op, "beep") == 0) {
        int32_t hz = (argc > 1) ? atoi(argv[1]) : 1000;
        int32_t ms = (argc > 2) ? atoi(argv[2]) : 200;
        start_beep(hz, ms);
        snprintf(buf, 128, "beep %d Hz %d ms\n", hz, ms);
    } else if (strcmp(op, "stop") == 0) {
        stop_sound();
        snprintf(buf, 128, "stopped\n");
    } else if (strcmp(op, "melody") == 0) {
        int32_t id = (argc > 1) ? atoi(argv[1]) : 1;
        start_melody(id);
        snprintf(buf, 128, "melody %d\n", _melody_id);
    } else if (strcmp(op, "volume") == 0) {
        if (argc > 1)
            ev3_sound_set_volume(atoi(argv[1]));
        snprintf(buf, 128, "volume %d\n", ev3_sound_get_volume());
    } else if (strcmp(op, "status") == 0) {
        beep_state_t st;
        get_state(&st);
        snprintf(buf, 128, "playing=%d freq=%d remain=%d volume=%d melody=%d\n",
                st.playing, st.freq_hz, st.remain_ms, st.volume, st.melody);
    } else {
        snprintf(buf, 128, "%s", usage);
    }
    return buf;
}

/* ---------------- timing ---------------- */

static int beep_loop(vdevice_t* dev, void* p) {
    (void)dev; (void)p;

    uint64_t now = kernel_tic_ms(0);

    /* One-shot beep timeout. */
    if (_tone_deadline > 0 && now >= _tone_deadline) {
        _tone_deadline = 0;
        play_freq(0);
    }

    /* Melody step. */
    if (_melody && _melody_next > 0 && now >= _melody_next) {
        _melody_idx++;
        const note_t* n = &_melody[_melody_idx];
        if (n->freq == 0 && n->ms == 0) {
            _melody = NULL;
            _melody_id = 0;
            play_freq(0);
        } else {
            play_freq(n->freq);
            _melody_next = now + (uint64_t)n->ms;
        }
    }

    proc_usleep(5000);   /* 5 ms tick is enough for beep timing */
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = "/dev/beep";
    if (argc > 1)
        mnt_point = argv[argc - 1];

    ev3_gpio_init();
    ev3_sound_init();
    ev3_sound_stop();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "beepd");
    dev.read = beep_read;
    dev.write = beep_write;
    dev.dev_cntl = beep_dev_cntl;
    dev.cmd = beep_cmd;
    dev.loop_step = beep_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    ev3_sound_stop();
    return 0;
}
