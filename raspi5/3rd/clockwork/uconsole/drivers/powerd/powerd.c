#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/klog.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/i2c.h>
#include <arch/bcm2712/mmio.h>

/*
 * The uConsole carrier wires the AXP223 PMU to the module's GPIO0/1, which
 * on BCM2712 are RP1 GPIO0/1 = RP1 i2c0 (funcsel a3). bcm2712_i2c_init()
 * maps the main + RP1 windows and muxes/pulls up those two pins itself, at
 * standard speed (100kHz) -- slow enough for the weak RP1 internal
 * pull-ups, and the PMU answers fine there.
 */
#define PMU_I2C_BUS   0
#define PMU_I2C_ADDR  0x34

static uint8_t  _hasBattery = 0;
static uint8_t  _charging = 0;
static uint8_t 	_capacity = 0;
static uint8_t  _gpio_pwr = 26; //uconsole power gpio = 26, devterm gpio = 2
/* set when the SDHCI host window could be mapped; power_off() then skips
 * the card power-down instead of faulting on an unmapped address */
static int _emmc_mapped = 0;

// 　　100%----4.20V
// 　　90%-----4.06V
// 　　80%-----3.98V
// 　　70%-----3.92V
// 　　60%-----3.87V
// 　　50%-----3.82V
// 　　40%-----3.79V
// 　　30%-----3.77V
// 　　20%-----3.74V
// 　　10%-----3.68V
// 　　5%------3.45V
// 　　0%------3.00V

static uint8_t adc2level(uint8_t adc){
    int vol = adc*72072/4095;
    if(vol > 4200)
        return 100;
    if(vol > 4060)
        return 90;
    if(vol > 3980)
        return 80;
    if(vol > 3920)
        return 70;
    if(vol > 3870)
        return 60;
    if(vol > 3820)
        return 50;
    if(vol > 3790)
        return 40;
    if(vol > 3770)
        return 30;
    if(vol > 3740)
        return 20;
    if(vol > 3680)
        return 10;
    if(vol > 3450)
        return 5;
    return 0;
}
/*
 * BCM2712 SD-card power-off.
 *
 * On the CM5 the SD card hangs off the dedicated sdio1 host at
 * 0x1000FFF000 (see arch_bcm2712/src/sdhci.c: ioaddr = _mmio_base +
 * PI5_EMMC_WIN_OFF + PI5_EMMC_OFF). Unlike the CM4 there are no GPIO48-53
 * SDIO lines to high-Z first -- the card sits on dedicated SoC pads, so the
 * host's own power control is the only thing that can cut card VDD.
 * SDHCI_POWER_CONTROL is byte register 0x29 and this host only accepts
 * 32-bit aligned accesses (SDHCI_QUIRK_REG32_RW), so clearing that byte
 * means read-modify-writing bits[15:8] of the word at offset 0x28.
 * Writing 0 removes card VDD, which force-resets the card back to idle --
 * the CM5 equivalent of the D1/AXP202 reference's "CMD0 + SD_VCC LDO off".
 * Without it the card can be caught mid-transfer and, if backfeed holds its
 * rail up after the PMIC cut, the next cold boot cannot talk to it until the
 * card is physically re-seated. Both the SD host (sdio1) and the WLAN SDIO
 * host (sdio2) are cleared; whichever is live powers down, the other is a
 * harmless no-op.
 */
static void sdhci_card_power_off(void) {
    if(!_emmc_mapped)
        return;
    static const uint32_t host_off[2] = { PI5_EMMC_OFF, PI5_WLAN_SDIO_OFF };
    ewokos_addr_t win = _mmio_base + PI5_EMMC_WIN_OFF;
    for(int h = 0; h < 2; h++) {
        ewokos_addr_t host = win + host_off[h];
        /*
         * Software reset (byte 0x2F bit0 = Reset For All) stops the host
         * from driving the SDIO pads and resets all registers to default,
         * including POWER_CONTROL to 0.  This is the BCM2712 equivalent of
         * the raspix GPIO48-53 high-Z step: on CM5 the SDIO pads are
         * dedicated SoC pins (not RP1 GPIOs), so the host reset is the only
         * way to disconnect the output drivers and prevent backfeed through
         * the card's ESD diodes into the collapsing VDD rail.
         *
         * Byte 0x2F sits in the 32-bit word at offset 0x2C, bits [31:24].
         * SDHCI_QUIRK_REG32_RW: this host only accepts 32-bit aligned access.
         */
        volatile uint32_t* rst = (volatile uint32_t*)(host + 0x2C);
        *rst |= (0x01U << 24);
        for(int t = 0; t < 10000; t++) {
            if (!(*rst & (0x01U << 24)))
                break;
        }
        /* Belt-and-suspenders: explicitly clear POWER_CONTROL (byte 0x29) */
        volatile uint32_t* pwr = (volatile uint32_t*)(host + 0x28);
        *pwr &= ~(0xffU << 8);
    }
}

/*
 * CM5 module self-power-off via the BCM2712 PM/watchdog block.
 *
 * KEY DIFFERENCE FROM CM4: on the uConsole CM4 the module's 5V comes from
 * AXP223-switched rails, so the AXP software power-off (REG 0x32) kills the
 * whole board.  The CM5 has its own onboard PMIC (DA9091) and its power
 * input is NOT gated by the AXP223 — the official Linux device tree for
 * uConsole CM5 does not even set "system-power-controller" on the AXP node.
 * So after the AXP cut, the display/wifi rails die (screen off) but the CM5
 * keeps running, and the board never reaches a wakeable off state.
 *
 * Linux powers the CM5 off the same way bcm2835-wdt does: write "partition
 * 63" (0x555 spread over PM_RSTS bits 0,2,4,6,8,10) and trigger a watchdog
 * full reset.  The VPU firmware sees partition 63, halts instead of
 * rebooting, and (with the CM5 default POWER_OFF_ON_HALT) commands the
 * DA9091 to switch off all module rails.  A later power-button press
 * cold-boots normally.
 *
 * The PM block sits at 0x10_7D200000, inside the main 64MB MMIO window
 * (0x10_7C000000) that mmio_map() already provides.
 */
#define PI5_PM_OFF               0x1200000  /* 0x107D200000 - 0x107C000000 */
#define PM_RSTC                  0x1c
#define PM_RSTS                  0x20
#define PM_WDOG                  0x24
#define PM_PASSWORD              0x5a000000
#define PM_RSTC_WRCFG_CLR        0xffffffcf
#define PM_RSTC_WRCFG_FULL_RESET 0x20
#define PM_RSTS_PARTITION_CLR    0xfffffaaa
#define PM_RSTS_PARTITION_HALT   0x555      /* partition 63 = halt/power-off */

static void cm5_module_power_off(void) {
    volatile uint32_t* rstc =
        (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_RSTC);
    volatile uint32_t* rsts =
        (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_RSTS);
    volatile uint32_t* wdog =
        (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_WDOG);

    uint32_t val = *rsts;
    val &= PM_RSTS_PARTITION_CLR;
    val |= PM_PASSWORD | PM_RSTS_PARTITION_HALT;
    *rsts = val;

    val = *rstc;
    val &= PM_RSTC_WRCFG_CLR;
    val |= PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
    *wdog = PM_PASSWORD | 10; /* ~150us until the reset fires */
    *rstc = val;
}

static void power_off(){
    /*
     * Release the power-button GPIO FIRST.  On the uConsole carrier GPIO26
     * is wired to the same net as the AXP223's PEK pin.  If we leave it
     * driven LOW (output) during the PMU's power-off sequence, the AXP223
     * sees PEK held and aborts/restarts the shutdown — the board never
     * reaches the "off" state and a subsequent physical press cannot wake
     * it (PEK is already LOW, no falling edge).
     *
     * On CM4 the SoC dies within a few ms of the PMU cutting rails, so
     * GPIO26 goes high-Z before the sequence completes.  On CM5 the RP1
     * has more bulk capacitance and survives longer, keeping the pin
     * driven LOW through the critical window.
     *
     * Setting INPUT + NO PULL makes the pad high-Z; the AXP223's internal
     * PEK pull-up (to VBAT, always-on) then holds the line HIGH = released.
     */
    bcm2712_gpio_pull(_gpio_pwr, GPIO_PULL_NONE);
    bcm2712_gpio_config(_gpio_pwr, GPIO_FUNC_INPUT);

    /*
     * Send the AXP223 power-off command while the RP1 i2c controller is
     * still in its known-good state (power_step just polled REG 0x4A).
     */
    for(int attempt = 0; attempt < 5; attempt++) {
        int reg = bcm2712_i2c_getb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x32);
        uint8_t val = (reg >= 0) ? (uint8_t)(reg | 0x80) : 0x80;
        if(bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x32, val) == 0)
            break;
        proc_usleep(2000);
    }

    /*
     * Clamp the i2c pins LOW to prevent ESD-diode backfeed from the
     * carrier's VBAT-tied pull-ups into the collapsing VDDIO rail.
     */
    bcm2712_gpio_pull(0, GPIO_PULL_NONE);
    bcm2712_gpio_pull(1, GPIO_PULL_NONE);
    bcm2712_gpio_write(0, false);
    bcm2712_gpio_write(1, false);
    bcm2712_gpio_config(0, GPIO_FUNC_OUTPUT);
    bcm2712_gpio_config(1, GPIO_FUNC_OUTPUT);

    /* Stop SDHCI hosts from driving the SDIO pads (SoC-side, not RP1). */
    sdhci_card_power_off();

    /*
     * Park remaining RP1 GPIOs low to minimise residual current.
     * Skip: 0/1 (i2c, already clamped above) and _gpio_pwr (PEK net,
     * must stay high-Z so the AXP223 sees button-released).
     */
    for(uint32_t i = 2; i < RP1_NUM_GPIOS; i++){
        if(i == _gpio_pwr)
            continue;
        bcm2712_gpio_write(i, false);
        bcm2712_gpio_config(i, GPIO_FUNC_OUTPUT);
    }

    /*
     * Finally power the CM5 module itself off.  Without this the module
     * keeps running from its own (non-AXP-gated) supply: screen dark but
     * system alive, power LED faintly lit, button unable to wake — the
     * exact difference from the CM4, whose rails all die with the AXP.
     */
    cm5_module_power_off();

    while(1);
}

/*
 * This board's PMIC is an AXP223, not an AXP202. REG 0x00 is the shared
 * AXP20X "power input status" register
 * (ACIN/VBUS/charge state) -- bit0 there has nothing to do with the
 * power key, which is why reading it never saw a press.
 *
 * AXP22X reports the power key ("PEK") as debounced, one-shot events
 * latched in the IRQ status registers, already enabled by the REG 0x42
 * write in main():
 *   IRQ3_STATE (0x4A) bit0 = PEK_LONG  (held past the long-press threshold)
 *   IRQ3_STATE (0x4A) bit1 = PEK_SHORT (pressed and released, short press)
 * The IRQ pin isn't wired to the SoC on this board, so these bits are
 * polled every power_step() tick and cleared (write-1-to-clear) by hand.
 * There is no live "is the key down right now" register on AXP22X, so a
 * latched event since the last poll is the best available signal.
 * Returns 0 = down(event seen), 1 = up(no event), matching the
 * "is_power_button_down() == 0" check in power_step().
 */
static int is_power_button_down(void) {
    int irq3 = bcm2712_i2c_getb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x4A);
    if(irq3 < 0)
        return 1; /* a bus error is NOT a key event: never power off on one */
    uint8_t pek = (uint8_t)irq3 & 0x3; /* bit0=PEK_LONG, bit1=PEK_SHORT */
    if(pek) {
        bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x4A, pek); /* write-1-to-clear, only what we saw */
        return 0; /* down */
    }
    return 1; /* up */
}

/*
 * Returns 0 when both PMU registers were read. On failure the last good
 * sample is kept, so a transient bus error can never be mistaken for an
 * empty battery (which power_step() would act on by powering off).
 */
static int power_sample(void) {
    int state = bcm2712_i2c_getb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x01);
    int adc = bcm2712_i2c_getb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x78);
    if(state < 0 || adc < 0)
        return -1;
    _hasBattery = !!(state & (0x1 << 5));
    _charging = !!(state & (0x1 << 6));
    _capacity = adc2level((uint8_t)adc);
    return 0;
}

static int power_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;	

    if(is_power_button_down() == 0){ //down
        power_off();
    }
    /* only a successfully-read capacity may trigger the shutdown */
    if(power_sample() == 0 && _capacity < 3) { //out of power
        //power_off();
    }
    proc_usleep(300000); 
    return 0;
}

static int power_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    uint8_t* data = (uint8_t *)buf;
    data[0] = _hasBattery;
    data[1] = _charging;
    data[2] = _capacity;
    return 3;
 }

static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
        c = getopt (argc, argv, "d");
        if(c == -1)
            break;

        switch (c) {
        case 'd':
            _gpio_pwr = 2;
            break;
        default:
            c = -1;
            break;
        }
    }
    return optind;
}

static char* power_help(void) {
    const char* usage =
        "usage: dev.cmd /dev/power0 <cmd>\n"
        "  help     show this help\n"
        "  status   re-sample PMU and show battery/charging/capacity/gpio\n"
        "  off      force power off\n";
    char* ret = (char*)malloc(strlen(usage) + 1);
    if(ret != NULL)
        strcpy(ret, usage);
    return ret;
}

static char* power_status(void) {
    int ok = (power_sample() == 0);
    char buf[160];
    snprintf(buf, sizeof(buf),
            "battery=%s charging=%s capacity=%u%% gpio_pwr=%u pmu=%s\n",
            _hasBattery ? "yes" : "no",
            _charging ? "yes" : "no",
            (unsigned)_capacity,
            (unsigned)_gpio_pwr,
            ok ? "ok" : "i2c error");
    char* ret = (char*)malloc(strlen(buf) + 1);
    if(ret != NULL)
        strcpy(ret, buf);
    return ret;
}

static char* power_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev;
    (void)from_pid;
    (void)p;

    if(argc <= 0 || argv == NULL || argv[0] == NULL)
        return NULL;

    if(strcmp(argv[0], "help") == 0)
        return power_help();

    if(strcmp(argv[0], "status") == 0)
        return power_status();

    if(strcmp(argv[0], "off") == 0) {
        power_off();
        return NULL;
    }

    return NULL;
}


int main(int argc, char** argv) {
    _gpio_pwr = 26;
    int32_t argind =  doargs(argc, argv);

    _mmio_base = mmio_map();
    if(_mmio_base == 0)
        return -1;

    bcm2712_gpio_init();
    bcm2712_gpio_config(_gpio_pwr, GPIO_FUNC_INPUT);
    bcm2712_gpio_pull(_gpio_pwr, GPIO_PULL_UP);

    if(bcm2712_i2c_init(PMU_I2C_BUS) != 0) {
        klog("powerd: i2c%d init failed\n", PMU_I2C_BUS);
        return -1;
    }
    /*
     * The SDHCI host window is only touched by power_off(); map it once here
     * so the shutdown path never has to syscall and never faults on an
     * unmapped address. Failure is not fatal -- the card power-down is then
     * simply skipped and the PMIC still cuts the board.
     */
    _emmc_mapped = (syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_EMMC_WIN_OFF,
            PI5_EMMC_PHY_WIN,
            PI5_EMMC_WIN_SIZE) == _mmio_base + PI5_EMMC_WIN_OFF);
    /*
     * REG 0x36 bits[1:0] = AXP22X hardware long-press force-poweroff delay
     * (0=4s, 1=6s, 2=8s, 3=10s) -- unlike AXP202's 3-bit field, AXP22X has
     * no "disable" setting here, so set the longest delay available. This
     * gives power_off() the most time to run a clean shutdown before the
     * PMIC would force VCC off by itself on a very long hold.
     */
    {
        int pek_key = bcm2712_i2c_getb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x36);
        if(pek_key >= 0)
            bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x36,
                    (uint8_t)((pek_key & ~0x3) | 0x3));
        else
            klog("powerd: no AXP223 answer on i2c%d addr 0x%02x\n",
                    PMU_I2C_BUS, PMU_I2C_ADDR);
    }
    bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x42, 0x3);
    /*
     * Discard any PEK_LONG/PEK_SHORT event already latched before we
     * started polling (e.g. the button press that powered the board on),
     * so it isn't mistaken for a fresh press on the first power_step().
     */
    bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x4A, 0x3);
    bcm2712_i2c_putb(PMU_I2C_BUS, PMU_I2C_ADDR, 0x82, 0x80);
    const char* mnt_point = "/dev/power0";
    if(argind < argc) {
        mnt_point = argv[argind];
        argind++;
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "powerd");
    dev.loop_step = power_step;
    dev.read = power_read;
    dev.cmd = power_dev_cmd;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
