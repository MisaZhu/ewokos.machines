#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/interrupt.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2c.h>

static uint8_t  _hasBattery = 0;
static uint8_t  _charging = 0;
static uint8_t 	_capacity = 0;
static uint8_t  _gpio_pwr = 26; //uconsole power gpio = 26, devterm gpio = 2

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
 * CM4/SDHCI SD-card power-off.
 *
 * The uConsole CM4 boots its SD card from the EMMC2 SDHCI host (see
 * system/libs/arch_bcm283x/src/sdhci.c: GPIO48-53 ALTF3, ioaddr =
 * _mmio_base + 0x340000). SDHCI_POWER_CONTROL is byte register 0x29 and
 * the bcm2711 host only accepts 32-bit aligned accesses, so clearing that
 * byte means read-modify-writing bits[15:8] of the word at offset 0x28.
 * Writing 0 removes card VDD, which force-resets the card back to idle --
 * the CM4 equivalent of the D1/AXP202 reference's "CMD0 + SD_VCC LDO off".
 * Without it the card can be caught mid-transfer and, if backfeed holds its
 * rail up after the PMIC cut, the next cold boot cannot talk to it until the
 * card is physically re-seated (exactly the reported symptom: powers off,
 * LED comes back on, but no boot until the SD card is pulled). Both the
 * EMMC2 (0x340000) and legacy (0x300000) hosts are cleared; whichever holds
 * the card powers down, the other is a harmless no-op.
 */
static void sdhci_card_power_off(void) {
    static const uint32_t host_off[2] = { 0x340000, 0x300000 };
    for(int h = 0; h < 2; h++) {
        volatile uint32_t* pwr =
            (volatile uint32_t*)(_mmio_base + host_off[h] + 0x28);
        *pwr &= ~(0xffU << 8); /* byte 0x29 = SDHCI_POWER_CONTROL -> 0 */
    }
}

static void power_off(){
    /*
     * Shutdown order mirrors the reference sequence, adapted to this board's
     * real parts (CM4 + AXP223, NOT D1 + AXP202):
     *
     * Step 3 - SDIO IO to true high-Z FIRST. GPIO48-53 are the SD card's
     *   CLK/CMD/DAT lines (ALTF3 on the CM4 EMMC2 host). Setting the pin MUX
     *   to input + no-pull disconnects the SoC's output drivers from the
     *   pads, so the SoC can no longer backfeed a collapsing card rail
     *   through the card's ESD diodes (the reason a re-seated card used to be
     *   needed). Every other pin is driven low to minimise residual current,
     *   except the AXP223 I2C bus (GPIO0/1), which is left idle-high so the
     *   Step 6 PMIC command still gets through.
     */
    for(int i = 0; i < 60 ; i++){
        if(i >= 48 && i <= 53) {
            bcm283x_gpio_pull(i, GPIO_PULL_NONE);
            bcm283x_gpio_config(i, GPIO_INPUT);
        } else if(i != 0 && i != 1) {
            bcm283x_gpio_clr(i);
            bcm283x_gpio_config(i, GPIO_OUTPUT);
        }
    }

    /*
     * Steps 1/2/4 - cut the card's own VDD at the SDHCI host. With the IO
     * lines already high-Z, removing card power cleanly force-resets the card
     * to idle, so the next power-on cold-boots from a card in a known state
     * instead of one stranded mid-operation.
     */
    sdhci_card_power_off();

    /* Step 5 - let the card rail and bus caps discharge before the PMIC cut. */
    proc_usleep(20000); /* 20 ms */

    /*
     * Step 6 - AXP223 software power-off (REG 0x32 bit7). i2c_do_start()
     * reconfigures GPIO0/1 on every transaction, so the bit-banged bus is
     * still usable here even though the pin walk above ran first.
     */
    uint8_t reg = i2c_getb(0x34, 0x32);
    i2c_putb(0x34, 0x32, reg | 0x80);
    while(1);
}

/*
 * This board's PMIC is an AXP223 (see drivers/dsi/uc_pmu.h), not an
 * AXP202. REG 0x00 is the shared AXP20X "power input status" register
 * (ACIN/VBUS/charge state) -- bit0 there has nothing to do with the
 * power key, which is why reading it never saw a press.
 *
 * AXP22X reports the power key ("PEK") as debounced, one-shot events
 * latched in the IRQ status registers, already enabled in main() via
 * i2c_putb(0x34, 0x42, 0x3):
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
    uint8_t irq3 = i2c_getb(0x34, 0x4A);
    uint8_t pek = irq3 & 0x3; /* bit0=PEK_LONG, bit1=PEK_SHORT */
    if(pek) {
        i2c_putb(0x34, 0x4A, pek); /* write-1-to-clear, only what we saw */
        return 0; /* down */
    }
    return 1; /* up */
}

static void power_sample(void) {
    uint8_t state = i2c_getb(0x34, 0x01);
    _hasBattery = !!(state & (0x1 << 5));
    _charging = !!(state & (0x1 << 6));
    _capacity = adc2level(i2c_getb(0x34, 0x78));
}

static int power_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;	

    if(is_power_button_down() == 0){ //down
        power_off();
    }
    power_sample();
    if(_capacity < 3) { //out of power
        power_off();
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
    power_sample();
    char buf[128];
    snprintf(buf, sizeof(buf),
            "battery=%s charging=%s capacity=%u%% gpio_pwr=%u\n",
            _hasBattery ? "yes" : "no",
            _charging ? "yes" : "no",
            (unsigned)_capacity,
            (unsigned)_gpio_pwr);
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

    ewokos_addr_t _mmio_base = mmio_map();
    if(_mmio_base == 0)
        return -1;

    bcm283x_gpio_init();
    bcm283x_gpio_config(_gpio_pwr, GPIO_INPUT);
    bcm283x_gpio_pull(_gpio_pwr, GPIO_PULL_UP);

    i2c_init(0,1);
    /*
     * REG 0x36 bits[1:0] = AXP22X hardware long-press force-poweroff delay
     * (0=4s, 1=6s, 2=8s, 3=10s) -- unlike AXP202's 3-bit field, AXP22X has
     * no "disable" setting here, so set the longest delay available. This
     * gives power_off() the most time to run a clean shutdown before the
     * PMIC would force VCC off by itself on a very long hold.
     */
    {
        uint8_t pek_key = i2c_getb(0x34, 0x36);
        i2c_putb(0x34, 0x36, (pek_key & ~0x3) | 0x3);
    }
    i2c_putb(0x34, 0x42, 0x3);
    /*
     * Discard any PEK_LONG/PEK_SHORT event already latched before we
     * started polling (e.g. the button press that powered the board on),
     * so it isn't mistaken for a fresh press on the first power_step().
     */
    i2c_putb(0x34, 0x4A, 0x3);
    i2c_putb(0x34, 0x82, 0x80);
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
