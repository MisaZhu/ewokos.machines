/*
 * Q8B cooling fan driver.
 *
 * The Radxa Dragon Q8B schematic connects FAN_PWM to SC8280XP GPIO118 and
 * drives the fan's low-side CJ3134K gate through R473.  There is no tach
 * return.  GPIO118 has no dedicated PWM function in the SC8280XP pinctrl,
 * so this small driver emits a conservative 50 Hz software PWM.  The Q8B
 * TSENS v2 block is sampled once per second and converted to a fan duty;
 * manual commands remain available for bring-up.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proto.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

#define GPIO118                    118U
#define TLMM_PHY                   0x0f100000ULL
#define TLMM_VA                    0x50700000ULL
#define TLMM_SIZE                  0x00300000U
#define TLMM_GPIO_CTL(gpio)        ((gpio) * 0x1000U)
#define TLMM_GPIO_IO(gpio)         (TLMM_GPIO_CTL(gpio) + 0x4U)
#define TLMM_GPIO_IO_OUT           (1U << 1)

#define TSENS_TM_PHY               0x0c263000ULL
#define TSENS_VA                    0x50b00000ULL
#define TSENS_SIZE                  0x00000200U
#define TSENS_STATUS                0x00a0U
#define TSENS_VALID                  (1U << 21)

/* TSENS0 SROT (Linux: second reg range of thermal-sensor@c263000).  The
 * AOP normally enables TSENS; if it did not, touching the TM block can
 * hang the interconnect, so probe SROT first. */
#define TSENS_SROT_PHY              0x0c222000ULL
#define TSENS_SROT_VA               0x50c00000ULL
#define TSENS_SROT_SIZE             0x00001000U
#define SROT_VER                    0x0000U
#define SROT_CTRL                   0x0004U
#define SROT_PERIOD                 0x0008U
#define SROT_TSENS_EN               (1U << 0)
#define SROT_SW_RST                 (1U << 1)
#define SROT_SENSOR_EN(id)          (1U << (3U + (id)))
#define SROT_CODE_OR_TEMP           (1U << 21)
#define TSENS_TRDY                  0x00e4U

#define FAN_PERIOD_US               20000U
#define FAN_LEVEL_MAX               10
#define FAN_DEFAULT_LEVEL           6
#define FAN_SAFE_DUTY               100

#define FAN_CNTL_SET_LEVEL          1
#define FAN_CNTL_GET_INFO           2

static volatile uint32_t* _tlmm;
static volatile uint32_t* _tsens_tm;
static volatile uint32_t* _tsens_srot;
static int _tsens_ok;
static pthread_mutex_t _lock;
static pthread_t _worker;
static int _level = FAN_DEFAULT_LEVEL;
static int _duty = FAN_SAFE_DUTY;
static int _temperature_mc;
static bool _temperature_valid;
static bool _automatic = true;
static bool _stop;

static void fan_gpio_set(bool on);

static int map_at(uint64_t va, uint64_t pa, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pa, size) == (ewokos_addr_t)va ? 0 : -1;
}

static inline uint32_t tlmm_read(uint32_t off) {
    return _tlmm[off / sizeof(uint32_t)];
}

static inline void tlmm_write(uint32_t off, uint32_t value) {
    _tlmm[off / sizeof(uint32_t)] = value;
}

static void fan_gpio_init(void) {
    uint32_t ctl = tlmm_read(TLMM_GPIO_CTL(GPIO118));

    /* Function 0 = GPIO, 8 mA drive, pull-down, output enabled.  The old
     * code selected function 1 (bit0) which may route an alternate signal
     * onto the fan pin; keep it as plain GPIO. */
    ctl &= ~((3U << 0) | (0xfU << 2) | (7U << 6) | (1U << 9));
    ctl |= (3U << 2) | (1U << 6) | (1U << 9);
    tlmm_write(TLMM_GPIO_CTL(GPIO118), ctl);
    fan_gpio_set(false);
}

static void fan_gpio_set(bool on) {
    uint32_t io = tlmm_read(TLMM_GPIO_IO(GPIO118));

    /* GPIO_OUT is bit 1; bit 0 is the read-only GPIO_IN value. */
    if (on)
        io |= TLMM_GPIO_IO_OUT;
    else
        io &= ~TLMM_GPIO_IO_OUT;
    tlmm_write(TLMM_GPIO_IO(GPIO118), io);
}

static inline uint32_t srot_read(uint32_t off) {
    return _tsens_srot[off / sizeof(uint32_t)];
}

static inline void srot_write(uint32_t off, uint32_t value) {
    _tsens_srot[off / sizeof(uint32_t)] = value;
}

static int tsens_init(void) {
    uint32_t ver;
    uint32_t ctrl;
    uint32_t period;
    uint32_t i;

    if (_tsens_srot == NULL || _tsens_tm == NULL)
        return -1;
    ver = srot_read(SROT_VER);
    ctrl = srot_read(SROT_CTRL);
    period = srot_read(SROT_PERIOD);
    klog("q8b fand: srot ver=%08x ctrl=%08x period=%08x\n",
            (unsigned)ver, (unsigned)ctrl, (unsigned)period);
    if (ver == 0U || ver == 0xffffffffU)
        return -1;

    if ((ctrl & SROT_TSENS_EN) == 0) {
        /* AOP did not enable TSENS.  Writing SROT or touching the TM block
         * in this state hangs the interconnect (verified on Q8B), so stay
         * strictly read-only and let the caller use a safe fixed duty. */
        klog("q8b fand: tsens disabled by AOP; fixed duty, TM untouched\n");
        return -1;
    }

    for (i = 0; i < 100U; i++) {
        if (_tsens_tm[TSENS_TRDY / sizeof(uint32_t)] & 1U)
            break;
        usleep(1000);
    }
    klog("q8b fand: trdy=%08x\n",
            (unsigned)_tsens_tm[TSENS_TRDY / sizeof(uint32_t)]);
    return 0;
}

static int tsens_read_sensor(uint32_t sensor, int* temp_mc) {
    uint32_t status;
    int raw;

    if (sensor >= 14)
        return -1;
    status = _tsens_tm[(TSENS_STATUS + sensor * 4U) / sizeof(uint32_t)];
    if ((status & TSENS_VALID) == 0)
        return -1;
    raw = (int)(status & 0xfffU);
    if (raw & 0x800)
        raw |= ~0xfff;
    /* RESULT_FORMAT_TEMP is deci-Celsius on TSENS v2. */
    *temp_mc = raw * 100;
    return (*temp_mc > -40000 && *temp_mc < 150000) ? 0 : -1;
}

static int tsens_read_cpu_temp(int* temp_mc) {
    int max_mc = -40000;
    int value;

    /* tsens0 sensors 1..8 are the eight CPU zones; sensor 9 is the cluster. */
    for (uint32_t sensor = 1; sensor <= 9; sensor++) {
        if (tsens_read_sensor(sensor, &value) == 0 && value > max_mc)
            max_mc = value;
    }
    if (max_mc == -40000)
        return -1;
    *temp_mc = max_mc;
    return 0;
}

static int temperature_to_duty(int temp_mc) {
    static const int temps[] = { 45000, 55000, 65000, 75000, 85000 };
    static const int duties[] = { 15, 30, 50, 75, 100 };

    if (temp_mc <= temps[0])
        return duties[0];
    for (unsigned i = 1; i < sizeof(temps) / sizeof(temps[0]); i++) {
        if (temp_mc < temps[i]) {
            int dt = temps[i] - temps[i - 1];
            int dd = duties[i] - duties[i - 1];
            return duties[i - 1] + (temp_mc - temps[i - 1]) * dd / dt;
        }
    }
    return FAN_SAFE_DUTY;
}

static void fan_set_duty_locked(int duty) {
    if (duty < 0)
        duty = 0;
    if (duty > 100)
        duty = 100;
    _duty = duty;
    _level = (duty * FAN_LEVEL_MAX + 50) / 100;
}

static void* fan_worker(void* unused) {
    (void)unused;
    unsigned tick = 0;

    while (!_stop) {
        int duty;
        int temp;

        pthread_mutex_lock(&_lock);
        duty = _duty;
        pthread_mutex_unlock(&_lock);

        if (duty <= 0) {
            fan_gpio_set(false);
            usleep(FAN_PERIOD_US);
        } else if (duty >= 100) {
            fan_gpio_set(true);
            usleep(FAN_PERIOD_US);
        } else {
            fan_gpio_set(true);
            usleep((unsigned)(FAN_PERIOD_US * duty / 100));
            fan_gpio_set(false);
            usleep((unsigned)(FAN_PERIOD_US - FAN_PERIOD_US * duty / 100));
        }

        if (++tick < 50)
            continue;
        tick = 0;
        if (!_tsens_ok) {
            pthread_mutex_lock(&_lock);
            if (_automatic)
                fan_set_duty_locked(60);
            pthread_mutex_unlock(&_lock);
            continue;
        }
        if (tsens_read_cpu_temp(&temp) == 0) {
            pthread_mutex_lock(&_lock);
            _temperature_mc = temp;
            _temperature_valid = true;
            if (_automatic)
                fan_set_duty_locked(temperature_to_duty(temp));
            pthread_mutex_unlock(&_lock);
        } else {
            /* A failed temperature read is a thermal safety event. */
            pthread_mutex_lock(&_lock);
            _temperature_valid = false;
            if (_automatic)
                fan_set_duty_locked(FAN_SAFE_DUTY);
            pthread_mutex_unlock(&_lock);
        }
    }
    fan_gpio_set(false);
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
            automatic ? "auto" : "manual", level, duty,
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
        fan_set_duty_locked(level * 100 / FAN_LEVEL_MAX);
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "duty") == 0 && argc > 1) {
        pthread_mutex_lock(&_lock);
        _automatic = false;
        fan_set_duty_locked(atoi(argv[1]));
        pthread_mutex_unlock(&_lock);
        return fan_status();
    }
    if (strcmp(argv[0], "temp") == 0) {
        return fan_status();
    }
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
    pthread_mutex_lock(&_lock);
    _automatic = false;
    fan_set_duty_locked((int)((const uint8_t*)buf)[0] * 100 / FAN_LEVEL_MAX);
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
        fan_set_duty_locked(level * 100 / FAN_LEVEL_MAX);
        pthread_mutex_unlock(&_lock);
        PF->addi(ret, level);
        return 0;
    }
    if (cmd == FAN_CNTL_GET_INFO) {
        pthread_mutex_lock(&_lock);
        PF->addi(ret, _level)->addi(ret, _duty)->addi(ret, _temperature_mc);
        pthread_mutex_unlock(&_lock);
        return 0;
    }
    return -1;
}

static void test_gpio(void) {
    if (map_at(TLMM_VA, TLMM_PHY, TLMM_SIZE) != 0) {
        klog("q8b fand: gpio map failed\n");
        return;
    }
    _tlmm = (volatile uint32_t*)(uintptr_t)TLMM_VA;
    klog("q8b fand: gpio init\n");
    fan_gpio_init();
    for (int i = 0; i < 6; i++) {
        klog("q8b fand: gpio on %d\n", i);
        fan_gpio_set(true);
        usleep(300000);
        klog("q8b fand: gpio off %d\n", i);
        fan_gpio_set(false);
        usleep(300000);
    }
    klog("q8b fand: gpio test done\n");
}

static void test_tsens(void) {
    if (map_at(TSENS_VA, TSENS_TM_PHY, TSENS_SIZE) != 0 ||
            map_at(TSENS_SROT_VA, TSENS_SROT_PHY, TSENS_SROT_SIZE) != 0) {
        klog("q8b fand: tsens map failed\n");
        return;
    }
    _tsens_tm = (volatile uint32_t*)(uintptr_t)TSENS_VA;
    _tsens_srot = (volatile uint32_t*)(uintptr_t)TSENS_SROT_VA;
    klog("q8b fand: tsens test start (read-only)\n");
    if (tsens_init() != 0) {
        klog("q8b fand: tsens unavailable; TM not touched\n");
        return;
    }
    for (uint32_t sensor = 1; sensor <= 9; sensor++) {
        uint32_t st = _tsens_tm[(TSENS_STATUS + sensor * 4U) /
                sizeof(uint32_t)];
        klog("q8b fand: sensor %u status=%08x\n", (unsigned)sensor,
                (unsigned)st);
    }
    klog("q8b fand: tsens test done\n");
}

int main(int argc, char** argv) {
    const char* mount = argc > 1 ? argv[1] : "/dev/fan";
    /* Bring-up isolation: keep cooling without probing either TSENS block.
     * Some boot states hang on the first TM read even with TSENS_EN set. */
    bool fixed = argc > 2 && strcmp(argv[2], "--fixed") == 0;
    vdevice_t dev;

    if (argc > 1 && strcmp(argv[1], "gpio-test") == 0) {
        test_gpio();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "tsens-test") == 0) {
        test_tsens();
        return 0;
    }

    if (map_at(TLMM_VA, TLMM_PHY, TLMM_SIZE) != 0 ||
            (!fixed && (map_at(TSENS_VA, TSENS_TM_PHY, TSENS_SIZE) != 0 ||
            map_at(TSENS_SROT_VA, TSENS_SROT_PHY, TSENS_SROT_SIZE) != 0))) {
        klog("q8b fand: MMIO map failed; fan left off\n");
        return -1;
    }
    _tlmm = (volatile uint32_t*)(uintptr_t)TLMM_VA;
    if (!fixed) {
        _tsens_tm = (volatile uint32_t*)(uintptr_t)TSENS_VA;
        _tsens_srot = (volatile uint32_t*)(uintptr_t)TSENS_SROT_VA;
    }
    pthread_mutex_init(&_lock, NULL);
    fan_gpio_init();
    if (fixed) {
        _automatic = false;
        klog("q8b fand: fixed full duty; TSENS not mapped or accessed\n");
    } else if (tsens_init() == 0)
        _tsens_ok = 1;
    else
        klog("q8b fand: no safe TSENS; fixed duty mode\n");
    fan_set_duty_locked(fixed ? FAN_SAFE_DUTY : FAN_DEFAULT_LEVEL * 100 / FAN_LEVEL_MAX);
    if (pthread_create(&_worker, NULL, fan_worker, NULL) != 0) {
        klog("q8b fand: worker create failed\n");
        fan_gpio_set(false);
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
