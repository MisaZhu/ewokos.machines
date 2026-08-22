/*
 * fand.c
 *
 * Raspberry Pi 5 cooling fan driver (J12 connector, JST-SH 4P 1.0mm).
 *
 * Connector pinout (socket, board side):
 *   1  +5V   red    supply, shared 5V rail, keep <= 500mA
 *   2  PWM   blue   RP1 PWM1 channel 3, 24 kHz, inverted polarity
 *   3  GND   black
 *   4  TACH  yellow FG pulses, counted by RP1 M-core firmware
 *
 * Both PWM and TACH live in the RP1 southbridge, not in the BCM2712 SoC.
 * The register layout below matches linux drivers/pwm/pwm-rp1.c and the
 * official rp1.dtsi/bcm2712-rpi-5-b.dts:
 *
 *   rp1_pwm1: pwm@9c000, clocked by RP1_CLK_PWM1 at 50 MHz
 *   GLB_CTRL    0x000   bit(chan) = channel enable, bit31 = SET_UPDATE
 *   CHAN_CTRL(c)0x014+c*0x10  bits1:0 mode, bit3 polarity, bit8 FIFO_POP
 *   RANGE(c)    0x018+c*0x10  period in clock ticks
 *   PHASE(c)    0x01C+c*0x10
 *   DUTY(c)     0x020+c*0x10
 *
 * The fan PWM pin is RP1 GPIO45, funcsel 0 ("pwm1", rp1_pwm1_gpio45 in
 * rp1.dtsi) with bias-pull-down. The EEPROM bootloader leaves it muxed
 * that way, but the pin is re-muxed here anyway: the OS must not rely on
 * bootloader residue.
 *
 * Polarity: the fan connector is inverted (CTRL.POLARITY = 1, same as
 * PWM_POLARITY_INVERTED in the official device tree): on the fan pin duty
 * 0% means full speed and duty 100% means stopped. With the hardware
 * polarity bit set, programming duty_ticks = level * RANGE / MAX therefore
 * yields level 0 = stopped and level MAX = full speed. Level 0 additionally
 * gates the channel off entirely (fan_commit), the official "disabled" path.
 * If a particular fan turns out to spin the other way, flip it at runtime
 * with "dev.cmd /dev/fan rev".
 *
 * RPM: RP1's M-core firmware counts the tachometer pulses and publishes
 * the result in the channel 2 PHASE register (offset 0x3C). That is how
 * the official device tree exposes it (rpm-regmap = rp1_pwm1,
 * rpm-offset = <0x3c>), so no GPIO edge counting is needed here.
 */

#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>

/* RP1 PWM1 block, inside the RP1 register file */
#define RP1_PWM1_OFF            0x9c000

#define RP1_PWM_GLB_CTRL        0x000
#define RP1_PWM_GLB_CTRL_EN(ch) (1u << (ch))
#define RP1_PWM_GLB_CTRL_UPDATE (1u << 31)

#define RP1_PWM_CHAN_CTRL(ch)   (0x014 + (ch) * 0x10)
#define RP1_PWM_CTRL_MODE_TE_MS 0x1       /* trailing edge, M/S modulation */
#define RP1_PWM_CTRL_POLARITY   (1u << 3) /* inverted output */
#define RP1_PWM_CTRL_FIFO_POP   (1u << 8)

#define RP1_PWM_RANGE(ch)       (0x018 + (ch) * 0x10)
#define RP1_PWM_PHASE(ch)       (0x01C + (ch) * 0x10)
#define RP1_PWM_DUTY(ch)        (0x020 + (ch) * 0x10)

/* RPM value published by the RP1 M-core firmware for the fan tach */
#define RP1_PWM_RPM             RP1_PWM_PHASE(2)

/*
 * RP1 clock controller (rp1.dtsi clocks@18000). RP1_CLK_PWM1 feeds the PWM
 * block; the linux pwm-rp1 driver clk_prepare_enable()s it in probe because
 * nothing guarantees it runs. From its aux source list (xosc is index 2),
 * dts asks for 50 MHz, which xosc delivers with a unity divider.
 */
#define RP1_CLOCKS_OFF          0x18000
#define RP1_CLK_PWM1_CTRL       0x084
#define RP1_CLK_PWM1_DIV_INT    0x088
#define RP1_CLK_PWM1_DIV_FRAC   0x08c
#define RP1_CLK_PWM1_SEL        0x090
#define RP1_CLK_CTRL_ENABLE     (1u << 11)
#define RP1_CLK_CTRL_AUXSRC(x)  ((x) << 5)
#define RP1_CLK_CTRL_SRC_AUX    0x1     /* SRC field = AUX_SEL */
#define RP1_CLK_SRC_MASK        0x1f
#define RP1_CLK_PWM1_SRC_XOSC   2

#define FAN_PWM_CHAN            3
#define FAN_PWM_GPIO            45
#define FAN_PWM_CLK_HZ          50000000
#define FAN_PWM_PERIOD_NS       41566     /* ~24 kHz */
#define FAN_PWM_RANGE_TICKS     ((uint32_t)(((uint64_t)FAN_PWM_CLK_HZ * \
                 FAN_PWM_PERIOD_NS) / 1000000000ULL))

#define FAN_LEVEL_MAX           10
#define FAN_LEVEL_DEFAULT       5

/* dev_cntl commands */
#define FAN_CNTL_SET_LEVEL      1
#define FAN_CNTL_GET_INFO       2

static ewokos_addr_t _pwm_base;
static int _fan_level = FAN_LEVEL_DEFAULT;
static int _fan_duty_pct = -1;   /* -1 = derived from level */
static bool _fan_rev = false;    /* flip level->duty conversion */

static uint32_t fan_level_to_duty_ticks(int level) {
    uint32_t pct;
    if (_fan_rev)
        pct = (uint32_t)((FAN_LEVEL_MAX - level) * 100 / FAN_LEVEL_MAX);
    else
        pct = (uint32_t)(level * 100 / FAN_LEVEL_MAX);
    return (uint32_t)(((uint64_t)FAN_PWM_RANGE_TICKS * pct) / 100);
}

/* commit the duty and channel state into the running PWM */
static void fan_commit(void) {
    uint32_t glb = get32(_pwm_base + RP1_PWM_GLB_CTRL);
    /*
     * Level 0 stops the fan the way the official pwm-rp1 driver disables
     * a PWM: gate the channel off and let the pin fall to its pull-down.
     * That is polarity independent, so "0 = stopped" holds either way.
     */
    if (_fan_level == 0 && _fan_duty_pct < 0)
        glb &= ~RP1_PWM_GLB_CTRL_EN(FAN_PWM_CHAN);
    else
        glb |= RP1_PWM_GLB_CTRL_EN(FAN_PWM_CHAN);
    put32(_pwm_base + RP1_PWM_GLB_CTRL, glb | RP1_PWM_GLB_CTRL_UPDATE);
}

static void fan_write_duty_ticks(uint32_t ticks) {
    if (ticks > FAN_PWM_RANGE_TICKS)
        ticks = FAN_PWM_RANGE_TICKS;
    put32(_pwm_base + RP1_PWM_DUTY(FAN_PWM_CHAN), ticks);
    fan_commit();
}

static void fan_set_level(int level) {
    if (level < 0)
        level = 0;
    if (level > FAN_LEVEL_MAX)
        level = FAN_LEVEL_MAX;
    _fan_level = level;
    _fan_duty_pct = -1;
    fan_write_duty_ticks(fan_level_to_duty_ticks(level));
}

static void fan_set_duty_pct(int pct) {
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    _fan_duty_pct = pct;
    fan_write_duty_ticks((uint32_t)(((uint64_t)FAN_PWM_RANGE_TICKS * pct) / 100));
}

static int fan_current_duty_pct(void) {
    if (_fan_duty_pct >= 0)
        return _fan_duty_pct;
    uint32_t ticks = fan_level_to_duty_ticks(_fan_level);
    return (int)((uint64_t)ticks * 100 / FAN_PWM_RANGE_TICKS);
}

static uint32_t fan_read_rpm(void) {
    return get32(_pwm_base + RP1_PWM_RPM);
}

static void fan_clk_init(void) {
    ewokos_addr_t clk = _mmio_base + PI5_RP1_WIN_OFF + RP1_CLOCKS_OFF;

    uint32_t ctrl = get32(clk + RP1_CLK_PWM1_CTRL);
    if (ctrl & RP1_CLK_CTRL_ENABLE)
        return; /* bootloader left it running, keep its setup */

    /*
     * Feed the PWM block with RP1_CLK_PWM1: xosc (50 MHz) at unity
     * divide, exactly what assigned-clocks in rp1.dtsi asks for. If the
     * bootloader left this clock gated (fan off at boot), the PWM core
     * cannot produce a waveform at all and every register write is
     * silently ignored by the fan.
     */
    put32(clk + RP1_CLK_PWM1_DIV_INT, 1);
    put32(clk + RP1_CLK_PWM1_DIV_FRAC, 0);
    ctrl &= ~(RP1_CLK_CTRL_AUXSRC(0x1f) | RP1_CLK_SRC_MASK);
    ctrl |= RP1_CLK_CTRL_SRC_AUX | RP1_CLK_CTRL_AUXSRC(RP1_CLK_PWM1_SRC_XOSC);
    put32(clk + RP1_CLK_PWM1_CTRL, ctrl);
    put32(clk + RP1_CLK_PWM1_CTRL, ctrl | RP1_CLK_CTRL_ENABLE);
}

static void fan_hw_init(void) {
    _pwm_base = _mmio_base + PI5_RP1_WIN_OFF + RP1_PWM1_OFF;

    fan_clk_init();

    /* PWM1 on GPIO45, funcsel 0, pulled down as in rp1.dtsi */
    bcm2712_gpio_init();
    bcm2712_gpio_config(FAN_PWM_GPIO, GPIO_FUNC_ALTF0);
    bcm2712_gpio_pull(FAN_PWM_GPIO, GPIO_PULL_DOWN);

    /* full re-init of the PWM1 channel, ignore bootloader residue */
    put32(_pwm_base + RP1_PWM_RANGE(FAN_PWM_CHAN), FAN_PWM_RANGE_TICKS);
    put32(_pwm_base + RP1_PWM_DUTY(FAN_PWM_CHAN), FAN_PWM_RANGE_TICKS);
    put32(_pwm_base + RP1_PWM_CHAN_CTRL(FAN_PWM_CHAN),
            RP1_PWM_CTRL_FIFO_POP | RP1_PWM_CTRL_POLARITY |
            RP1_PWM_CTRL_MODE_TE_MS);
    fan_commit();
}

/*
 * Dump the whole hardware chain: PWM1 clock, GPIO45 mux/pad, PWM registers
 * and the M-core RPM. Reading DUTY twice with a gap in between also exposes
 * something else (firmware) rewriting the block behind our back.
 */
static char* fan_diag(void) {
    char* s = (char*)malloc(1024);
    if (s == NULL)
        return NULL;

    ewokos_addr_t clk = _mmio_base + PI5_RP1_WIN_OFF + RP1_CLOCKS_OFF;
    ewokos_addr_t gpio_ctrl = _mmio_base + RP1_IO_BANK_OFF +
        2 * RP1_BANK_STRIDE + (FAN_PWM_GPIO - 34) * RP1_GPIO_PIN_STRIDE +
        RP1_GPIO_CTRL;
    ewokos_addr_t gpio_pad = _mmio_base + RP1_PADS_BANK_OFF +
        2 * RP1_BANK_STRIDE + RP1_PADS_PIN0 +
        (FAN_PWM_GPIO - 34) * RP1_PAD_PIN_STRIDE;

    uint32_t duty0 = get32(_pwm_base + RP1_PWM_DUTY(FAN_PWM_CHAN));
    usleep(200000);
    uint32_t duty1 = get32(_pwm_base + RP1_PWM_DUTY(FAN_PWM_CHAN));

    int n = 0;
    n += snprintf(s + n, 1024 - n,
            "clk_pwm1: ctrl=0x%08x sel=0x%08x div=%u.%04x\n",
            get32(clk + RP1_CLK_PWM1_CTRL), get32(clk + RP1_CLK_PWM1_SEL),
            get32(clk + RP1_CLK_PWM1_DIV_INT),
            get32(clk + RP1_CLK_PWM1_DIV_FRAC) & 0xffff);
    n += snprintf(s + n, 1024 - n,
            "gpio45:   ctrl=0x%08x pad=0x%08x\n",
            get32(gpio_ctrl), get32(gpio_pad));
    n += snprintf(s + n, 1024 - n,
            "pwm1 ch%d: glb=0x%08x ctrl=0x%08x range=%u duty=%u->%u phase=%u\n",
            FAN_PWM_CHAN,
            get32(_pwm_base + RP1_PWM_GLB_CTRL),
            get32(_pwm_base + RP1_PWM_CHAN_CTRL(FAN_PWM_CHAN)),
            get32(_pwm_base + RP1_PWM_RANGE(FAN_PWM_CHAN)),
            duty0, duty1,
            get32(_pwm_base + RP1_PWM_PHASE(FAN_PWM_CHAN)));
    snprintf(s + n, 1024 - n,
            "rpm=%u level=%d%s\n", fan_read_rpm(), _fan_level,
            (duty0 != duty1) ? "  WARNING: DUTY rewritten by someone else" : "");
    return s;
}

static int fan_status_str(char* buf, int size) {
    const char* mode = _fan_duty_pct >= 0 ? "manual" : "level";
    return snprintf(buf, size, "fan %s %d duty %d%% rpm %u\n",
            mode,
            _fan_duty_pct >= 0 ? _fan_duty_pct : _fan_level,
            fan_current_duty_pct(), fan_read_rpm());
}

static char* fan_dup_status(void) {
    char buf[64];
    fan_status_str(buf, sizeof(buf));
    char* ret = (char*)malloc(strlen(buf) + 1);
    if (ret != NULL)
        strcpy(ret, buf);
    return ret;
}

static char* fan_help(void) {
    const char* usage =
        "usage: dev.cmd /dev/fan <cmd>\n"
        "  run <0-10>   set fan level (0=stopped 10=full)\n"
        "  status       show level/duty/rpm\n"
        "  duty <0-100> set raw PWM duty percent\n"
        "  rev          flip level<->duty conversion\n"
        "  diag         dump clock/gpio/pwm register state\n";
    char* ret = (char*)malloc(strlen(usage) + 1);
    if (ret != NULL)
        strcpy(ret, usage);
    return ret;
}

static char* fan_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;

    if (strcmp(argv[0], "help") == 0)
        return fan_help();

    if (strcmp(argv[0], "status") == 0)
        return fan_dup_status();

    if (strcmp(argv[0], "diag") == 0)
        return fan_diag();

    if (strcmp(argv[0], "run") == 0 && argc > 1) {
        char* end = NULL;
        long level = strtol(argv[1], &end, 10);
        if (end != NULL && *end == 0) {
            fan_set_level((int)level);
            return fan_dup_status();
        }
        return NULL;
    }

    if (strcmp(argv[0], "rev") == 0) {
        _fan_rev = !_fan_rev;
        if (_fan_duty_pct < 0)
            fan_set_level(_fan_level);
        char buf[64];
        snprintf(buf, sizeof(buf), "fan level->duty conversion %s\n",
                _fan_rev ? "reversed" : "normal");
        char* ret = (char*)malloc(strlen(buf) + 1);
        if (ret != NULL)
            strcpy(ret, buf);
        return ret;
    }

    if (strcmp(argv[0], "duty") == 0 && argc > 1) {
        int pct = atoi(argv[1]);
        fan_set_duty_pct(pct);
        return fan_dup_status();
    }

    return NULL;
}

static int fan_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        void* buf, int size, int offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)p;

    if (size <= 0 || offset != 0)
        return 0;

    ((uint8_t*)buf)[0] = (uint8_t)_fan_level;
    return 1;
}

static int fan_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        const void* buf, int size, int offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;

    if (size <= 0)
        return 0;

    fan_set_level((int)((const uint8_t*)buf)[0]);
    return 1;
}

static int fan_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in,
        proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    if (cmd == FAN_CNTL_SET_LEVEL) {
        int level = proto_read_int(in);
        fan_set_level(level);
        PF->addi(ret, _fan_level);
    }
    else if (cmd == FAN_CNTL_GET_INFO) {
        PF->addi(ret, _fan_level)->
            addi(ret, fan_current_duty_pct())->
            addi(ret, (int)fan_read_rpm());
    }
    else
        return -1;
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/fan";

    /*
     * mmio_map() only covers the main 64MB peripheral window; the RP1
     * register file lives in its own kernel window at PI5_RP1_WIN_OFF,
     * so map it explicitly like uartd does. Touching it unmapped aborts
     * this process and wedges init on the ipcserv line.
     */
    sys_info_t sysinfo;
    sys_get_sys_info(&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;
    syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size);
    if (syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_RP1_WIN_OFF,
            PI5_RP1_PHY,
            PI5_RP1_WIN_SIZE) != _mmio_base + PI5_RP1_WIN_OFF) {
        klog("fand: map RP1 window failed\n");
        return -1;
    }

    /*
     * The fan PWM/TACH live in the RP1 southbridge, whose register file at
     * PI5_RP1_PHY only decodes after the PCIe2 link is trained and RP1's
     * configuration/outbound window is enabled (see hw_arch.h). fand starts
     * very early in init.rd, before the drivers that normally bring RP1 up
     * (i2c/usbhostd) have run, so train/enable it here too. bcm2712_rp1_init()
     * is idempotent: if the bootloader or an earlier driver already left the
     * link up it only (re)confirms the BARs.
     */
    if (bcm2712_rp1_init() != 0)
        klog("fand: RP1 link not ready, fan may not respond\n");

    fan_hw_init();
    fan_set_level(FAN_LEVEL_DEFAULT);
    slog("fand: RP1 PWM1 ch%d, %d Hz inverted, level %d/%d\n",
            FAN_PWM_CHAN, 1000000000 / FAN_PWM_PERIOD_NS,
            _fan_level, FAN_LEVEL_MAX);

    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "fan");
    dev.read = fan_read;
    dev.write = fan_write;
    dev.cmd = fan_cmd;
    dev.dev_cntl = fan_dcntl;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
    return 0;
}
