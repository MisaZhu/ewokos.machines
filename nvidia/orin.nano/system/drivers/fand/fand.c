/*
 * Orin Nano fan daemon: SoC temperature -> hardware PWM3.
 *
 * Temperature comes from BPMP MRQ_THERMAL zone TJ_MAX (the vendor
 * tj-thermal control source) over the UEFI-established IVC channel.
 * The fan PWM is the Tegra234 PWM3 controller (tegra186 model: one
 * channel, CSR at +0, 256-tick duty, prescaler bits[12:0]).  Clock and
 * reset are owned via BPMP BEFORE any register access -- an unclocked
 * PWM MMIO read raises an EL3 SError on this board.
 *
 * Auto policy mirrors the NVIDIA p3767/p3768 fan tables: trips
 * 35/74/95 C, cooling levels 0/88/187/255 (of 255), 4 C hysteresis.
 * A failed temperature read drives full duty (thermal failsafe).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <bsp/bsp_bpmp.h>
#include <orin_net.h>

#define PWM_VA                  0x64000000ULL
#define PWM_CSR                 0x0u
#define PWM_ENABLE              (1u << 31)
#define PWM_DUTY_SHIFT          16
#define PWM_DEPTH               256u
#define PWM_SCALE_MAX           0x1fffu

#define FAN_LEVEL_MAX           10
#define FAN_DUTY_MAX            255
#define FAN_HYST_MC             4000

#define FAN_CNTL_SET_LEVEL      1
#define FAN_CNTL_GET_INFO       2

static volatile uint32_t* _pwm;
static uint32_t _scale;
static uint64_t _clk_rate;
static pthread_mutex_t _lock;
static pthread_t _worker;
static int _duty = FAN_DUTY_MAX;      /* raw 0..255 PWM ticks */
static int _level = FAN_LEVEL_MAX;
static int _temperature_mc;
static bool _temperature_valid;
static bool _automatic = true;
static bool _fixed;
static bool _stop;
static int _trip_idx = -1;

static int map_at(uint64_t va, uint64_t pa, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pa, size) == (ewokos_addr_t)va ? 0 : -1;
}

/* pwm-tegra.c config path for a 1-channel controller: the clock must
 * reach DEPTH ticks within the period; if the rounded rate is below
 * that lower bound, double the request so the prescaler can divide
 * down.  scale = clk*period/(1e9*DEPTH) - 1. */
static int pwm_clock_config(void) {
    uint64_t period = ORIN_FAN_PWM_PERIOD_NS;
    uint64_t need = (1000000000ULL * PWM_DEPTH + period - 1) / period;
    uint64_t got = 0;
    int r;

    /* dtsi assigned-clock-parents = <&bpmp TEGRA234_CLK_PLLP_OUT0>;
     * the boot parent (19.2 MHz) cannot reach ~22 kHz. */
    r = bsp_bpmp_clk_set_parent(ORIN_PWM3_CLK, ORIN_CLK_PLLP_OUT0);
    klog("orin fand: pwm3 set_parent(pllp_out0) = %d\n", r);

    r = bsp_bpmp_clk_round_rate(ORIN_PWM3_CLK, need, &got);
    if (r != 0) {
        klog("orin fand: pwm3 round_rate failed (%d)\n", r);
        return -1;
    }
    if (got < need)
        need *= 2;
    r = bsp_bpmp_clk_set_rate(ORIN_PWM3_CLK, need);
    if (r != 0) {
        klog("orin fand: pwm3 set_rate(%llu) failed (%d)\n",
                (unsigned long long)need, r);
        return -1;
    }
    r = bsp_bpmp_clk_get_rate(ORIN_PWM3_CLK, &_clk_rate);
    if (r != 0 || _clk_rate == 0) {
        klog("orin fand: pwm3 get_rate failed (%d)\n", r);
        return -1;
    }
    uint64_t scale = _clk_rate * period / (1000000000ULL * PWM_DEPTH);
    if (scale == 0 || scale - 1 > PWM_SCALE_MAX) {
        klog("orin fand: pwm3 rate %llu Hz unusable for %llu ns\n",
                (unsigned long long)_clk_rate, (unsigned long long)period);
        return -1;
    }
    _scale = (uint32_t)(scale - 1);
    klog("orin fand: pwm3 clk=%llu Hz scale=%u period=%llu ns\n",
            (unsigned long long)_clk_rate, (unsigned)_scale,
            (unsigned long long)(PWM_DEPTH * (_scale + 1) *
                1000000000ULL / _clk_rate));
    return 0;
}

static int pwm_init(void) {
    if (bsp_bpmp_init() != 0) {
        klog("orin fand: bpmp init failed\n");
        return -1;
    }
    if (pwm_clock_config() != 0)
        return -1;
    /* Clock on + reset deasserted BEFORE the first register access. */
    int r = bsp_bpmp_clk_enable(ORIN_PWM3_CLK);
    if (r != 0) {
        klog("orin fand: pwm3 clk_enable failed (%d)\n", r);
        return -1;
    }
    r = bsp_bpmp_reset_deassert(ORIN_PWM3_RESET);
    if (r != 0) {
        klog("orin fand: pwm3 reset_deassert failed (%d)\n", r);
        return -1;
    }
    if (map_at(PWM_VA, ORIN_PWM3_PHY, ORIN_PWM3_SIZE) != 0) {
        klog("orin fand: pwm3 MMIO map failed\n");
        return -1;
    }
    _pwm = (volatile uint32_t*)(uintptr_t)PWM_VA;
    klog("orin fand: pwm3 csr boot=%08x\n", (unsigned)_pwm[PWM_CSR / 4]);
    return 0;
}

static void pwm_apply(int duty) {
    if (duty < 0)
        duty = 0;
    /* 256 ticks would overflow the 8-bit duty field; 255/256 is full
     * speed for the fan. */
    if (duty > FAN_DUTY_MAX)
        duty = FAN_DUTY_MAX;
    uint32_t csr = ((uint32_t)duty << PWM_DUTY_SHIFT) | _scale;
    if (duty > 0)
        csr |= PWM_ENABLE;
    _pwm[PWM_CSR / 4] = csr;
}

/* NVIDIA tj-thermal trips with hysteresis on the way down. */
static int thermal_to_duty(int temp_mc) {
    static const int trips[] = { 35000, 74000, 95000 };
    static const int duties[] = { 88, 187, 255 };
    int idx = _trip_idx;

    while (idx < 2 && temp_mc >= trips[idx + 1])
        idx++;
    while (idx >= 0 && temp_mc < trips[idx] - FAN_HYST_MC)
        idx--;
    _trip_idx = idx;
    return idx < 0 ? 0 : duties[idx];
}

static void fan_set_duty_locked(int duty) {
    if (duty < 0)
        duty = 0;
    if (duty > FAN_DUTY_MAX)
        duty = FAN_DUTY_MAX;
    _duty = duty;
    _level = (duty * FAN_LEVEL_MAX + FAN_DUTY_MAX / 2) / FAN_DUTY_MAX;
}

static void* fan_worker(void* unused) {
    (void)unused;
    int last_applied = -1;

    while (!_stop) {
        sleep(1);
        if (!_fixed) {
            int temp;
            int r = bsp_bpmp_thermal_get_temp(ORIN_THERM_ZONE_TJ_MAX, &temp);
            pthread_mutex_lock(&_lock);
            if (r == 0 && temp > -40000 && temp < 150000) {
                _temperature_mc = temp;
                _temperature_valid = true;
                if (_automatic)
                    fan_set_duty_locked(thermal_to_duty(temp));
            } else {
                /* A failed temperature read is a thermal safety event. */
                _temperature_valid = false;
                if (_automatic)
                    fan_set_duty_locked(FAN_DUTY_MAX);
            }
            pthread_mutex_unlock(&_lock);
        }

        pthread_mutex_lock(&_lock);
        int duty = _duty;
        pthread_mutex_unlock(&_lock);
        if (duty != last_applied) {
            pwm_apply(duty);
            klog("orin fand: duty %d/255 (%d%%) csr=%08x\n", duty,
                    duty * 100 / FAN_DUTY_MAX,
                    (unsigned)_pwm[PWM_CSR / 4]);
            last_applied = duty;
        }
    }
    return NULL;
}

static char* dup_text(const char* text) {
    char* ret = (char*)malloc(strlen(text) + 1);
    if (ret != NULL)
        strcpy(ret, text);
    return ret;
}

static char* fan_status(void) {
    char buf[160];
    int level, duty, temp;
    bool automatic, valid;

    pthread_mutex_lock(&_lock);
    level = _level;
    duty = _duty;
    temp = _temperature_mc;
    automatic = _automatic;
    valid = _temperature_valid;
    pthread_mutex_unlock(&_lock);
    snprintf(buf, sizeof(buf), "fan %s level %d duty %d%% temp %s%d.%dC\n",
            automatic ? "auto" : "manual", level, duty * 100 / FAN_DUTY_MAX,
            valid ? "" : "?", temp / 1000, abs(temp % 1000) / 100);
    return dup_text(buf);
}

static char* fan_cmd(vdevice_t* dev, int from_pid, int argc, char** argv,
        void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (argc == 0 || strcmp(argv[0], "status") == 0)
        return fan_status();
    if (strcmp(argv[0], "auto") == 0) {
        pthread_mutex_lock(&_lock);
        _automatic = true;
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "manual") == 0) {
        pthread_mutex_lock(&_lock);
        _automatic = false;
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "run") == 0 && argc > 1) {
        int level = atoi(argv[1]);
        if (level < 0) level = 0;
        if (level > FAN_LEVEL_MAX) level = FAN_LEVEL_MAX;
        pthread_mutex_lock(&_lock);
        _automatic = false;
        fan_set_duty_locked(level * FAN_DUTY_MAX / FAN_LEVEL_MAX);
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "duty") == 0 && argc > 1) {
        pthread_mutex_lock(&_lock);
        _automatic = false;
        fan_set_duty_locked(atoi(argv[1]) * FAN_DUTY_MAX / 100);
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "temp") == 0)
        return fan_status();
    return dup_text("usage: status | auto | manual | run <0-10> | duty <0-100> | temp\n");
}

static int fan_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;
    if (size <= 0)
        return 0;
    pthread_mutex_lock(&_lock);
    ((uint8_t*)buf)[0] = (uint8_t)_level;
    pthread_mutex_unlock(&_lock);
    return 1;
}

static int fan_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;
    if (size <= 0)
        return 0;
    int level = ((const uint8_t*)buf)[0];
    if (level > FAN_LEVEL_MAX)
        level = FAN_LEVEL_MAX;
    pthread_mutex_lock(&_lock);
    _automatic = false;
    fan_set_duty_locked(level * FAN_DUTY_MAX / FAN_LEVEL_MAX);
    pthread_mutex_unlock(&_lock);
    return 1;
}

static int fan_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in,
        proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (cmd == FAN_CNTL_SET_LEVEL) {
        int level = proto_read_int(in);
        if (level < 0) level = 0;
        if (level > FAN_LEVEL_MAX) level = FAN_LEVEL_MAX;
        pthread_mutex_lock(&_lock);
        _automatic = false;
        fan_set_duty_locked(level * FAN_DUTY_MAX / FAN_LEVEL_MAX);
        pthread_mutex_unlock(&_lock);
        PF->addi(ret, level);
        return 0;
    }
    if (cmd == FAN_CNTL_GET_INFO) {
        pthread_mutex_lock(&_lock);
        PF->addi(ret, _level)->addi(ret, _duty * 100 / FAN_DUTY_MAX)
                ->addi(ret, _temperature_mc);
        pthread_mutex_unlock(&_lock);
        return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    const char* mount = argc > 1 ? argv[1] : "/dev/fan";
    _fixed = argc > 2 && strcmp(argv[2], "--fixed") == 0;
    vdevice_t dev;

    if (pwm_init() != 0) {
        klog("orin fand: pwm init failed; fan left at firmware default\n");
        return -1;
    }
    pthread_mutex_init(&_lock, NULL);
    if (_fixed) {
        _automatic = false;
        fan_set_duty_locked(FAN_DUTY_MAX);
        klog("orin fand: fixed full duty; thermal not queried\n");
    } else {
        int temp;
        if (bsp_bpmp_thermal_get_temp(ORIN_THERM_ZONE_TJ_MAX, &temp) == 0)
            klog("orin fand: tj_max boot temp %d mC\n", temp);
        else
            klog("orin fand: tj_max boot read failed; failsafe until ok\n");
        fan_set_duty_locked(FAN_DUTY_MAX);
    }
    pwm_apply(_duty);
    if (pthread_create(&_worker, NULL, fan_worker, NULL) != 0) {
        klog("orin fand: worker create failed\n");
        return -1;
    }
    pthread_detach(_worker);

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "fan");
    dev.read = fan_read;
    dev.write = fan_write;
    dev.cmd = fan_cmd;
    dev.dev_cntl = fan_dcntl;
    device_run(&dev, mount, FS_TYPE_CHAR, 0666, false);
    return 0;
}