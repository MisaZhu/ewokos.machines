#include <arch/bcm2712/gpio.h>
#include <ewoksys/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>

#if PI5_RP1_WIN_OFF != 0x06000000
#error "RP1 GPIO window must match the raspi5 kernel mapping"
#endif

/*
 * RP1 pin banks, from rp1_iobanks[] in linux drivers/pinctrl/pinctrl-rp1.c:
 *   Bank 0 (+0x0000): GPIO 0-27  (28 pins, the 40 pin header)
 *   Bank 1 (+0x4000): GPIO 28-33 (6 pins, ethernet/usb control)
 *   Bank 2 (+0x8000): GPIO 34-53 (20 pins, internal)
 *
 * A pin is described by one register in each of the three regions, so the
 * bases have to be tracked separately. They are virtual addresses inside the
 * RP1 window, which sits above 4GB on aarch64, hence ewokos_addr_t.
 */

static ewokos_addr_t _io_bank_base;
static ewokos_addr_t _sys_rio_base;
static ewokos_addr_t _pads_bank_base;
static uint8_t _gpio_ready;

static inline uint32_t gpio_bank(uint32_t pin) {
    if (pin < 28) return 0;
    if (pin < 34) return 1;
    return 2;
}

static inline uint32_t gpio_bank_pin(uint32_t pin) {
    if (pin < 28) return pin;
    if (pin < 34) return pin - 28;
    return pin - 34;
}

/* IO_BANK register pair of a pin */
static inline ewokos_addr_t gpio_ctrl(uint32_t pin) {
    return _io_bank_base + gpio_bank(pin) * RP1_BANK_STRIDE +
        gpio_bank_pin(pin) * RP1_GPIO_PIN_STRIDE + RP1_GPIO_CTRL;
}

/* SYS_RIO bitmap register of the bank a pin belongs to */
static inline ewokos_addr_t gpio_rio(uint32_t pin, uint32_t reg, uint32_t alias) {
    return _sys_rio_base + gpio_bank(pin) * RP1_BANK_STRIDE + reg + alias;
}

/* PADS_BANK register of a pin */
static inline ewokos_addr_t gpio_pad(uint32_t pin) {
    return _pads_bank_base + gpio_bank(pin) * RP1_BANK_STRIDE +
        RP1_PADS_PIN0 + gpio_bank_pin(pin) * RP1_PAD_PIN_STRIDE;
}

static inline void gpio_pad_update(uint32_t pin, uint32_t clr, uint32_t set) {
    ewokos_addr_t pad = gpio_pad(pin);
    uint32_t val = get32(pad);
    val &= ~clr;
    val |= set;
    put32(pad, val);
}

void bcm2712_gpio_init(void) {
    if(_gpio_ready)
        return;

    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;
    syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size);
    syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_RP1_WIN_OFF,
            PI5_RP1_PHY,
            PI5_RP1_WIN_SIZE);

    _io_bank_base   = _mmio_base + RP1_IO_BANK_OFF;
    _sys_rio_base   = _mmio_base + RP1_SYS_RIO_OFF;
    _pads_bank_base = _mmio_base + RP1_PADS_BANK_OFF;

    /*
     * GPIO can appear mapped while the RP1 BARs are still not fully usable.
     * Match SPI/I2C and make the RP1 bring-up explicit before touching IO_BANK.
     */
    if (bcm2712_rp1_init() != 0)
        return;

    _gpio_ready = 1;
}

void bcm2712_gpio_config(uint32_t pin, uint32_t func) {
    if (pin >= RP1_NUM_GPIOS) return;

    uint32_t fsel = func & RP1_GPIO_CTRL_FUNCSEL_MASK;

    /*
     * The direction has to be set before the function, otherwise the pin
     * drives whatever RIO_OE happened to hold. Same order as
     * rp1_gpio_direction_input()/_output() in linux.
     */
    if (func & GPIO_FUNC_DIR_FLAG) {
        uint32_t alias = (func == GPIO_FUNC_OUTPUT) ?
            RP1_SET_OFFSET : RP1_CLR_OFFSET;
        put32(gpio_rio(pin, RP1_RIO_OE, alias), 1 << gpio_bank_pin(pin));
        fsel = GPIO_FUNC_RIO;
    }

    /* The pad has to let the signal through in both directions */
    gpio_pad_update(pin, RP1_PAD_OUT_DISABLE_MASK, RP1_PAD_IN_ENABLE_MASK);

    uint32_t ctrl = get32(gpio_ctrl(pin));
    ctrl &= ~(RP1_GPIO_CTRL_FUNCSEL_MASK | RP1_GPIO_CTRL_OUTOVER_MASK |
            RP1_GPIO_CTRL_OEOVER_MASK);
    if (fsel == GPIO_FUNC_NONE) {
        /* No peripheral drives the pin: park it as an input */
        ctrl |= RP1_OEOVER_DISABLE << RP1_GPIO_CTRL_OEOVER_LSB;
        ctrl |= RP1_GPIO_CTRL_FUNCSEL_MASK;  /* FSEL_NONE_HW == 0x1f */
    } else {
        ctrl |= RP1_OUTOVER_PERI << RP1_GPIO_CTRL_OUTOVER_LSB;
        ctrl |= RP1_OEOVER_PERI << RP1_GPIO_CTRL_OEOVER_LSB;
        ctrl |= fsel << RP1_GPIO_CTRL_FUNCSEL_LSB;
    }
    put32(gpio_ctrl(pin), ctrl);
}

void bcm2712_gpio_pull(uint32_t pin, uint32_t pull) {
    if (pin >= RP1_NUM_GPIOS) return;

    gpio_pad_update(pin, RP1_PAD_PULL_MASK,
            (pull << RP1_PAD_PULL_LSB) & RP1_PAD_PULL_MASK);
}

void bcm2712_gpio_write(uint32_t pin, bool high) {
    if (pin >= RP1_NUM_GPIOS) return;

    uint32_t alias = high ? RP1_SET_OFFSET : RP1_CLR_OFFSET;
    put32(gpio_rio(pin, RP1_RIO_OUT, alias), 1 << gpio_bank_pin(pin));
}

bool bcm2712_gpio_read(uint32_t pin) {
    if (pin >= RP1_NUM_GPIOS) return false;

    uint32_t in = get32(gpio_rio(pin, RP1_RIO_IN, RP1_RW_OFFSET));
    return (in & (1 << gpio_bank_pin(pin))) != 0;
}
