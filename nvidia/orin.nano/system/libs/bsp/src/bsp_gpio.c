#include <stdint.h>
#include <bsp/bsp_gpio.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 GPIO controller (tegra186-gpio style).  Port->register mapping
 * from upstream gpio-tegra186.c tegra234_main_ports / tegra234_aon_ports:
 *   reg    = base   + bank*0x1000 + port*0x200 + pin*0x20
 *   secure = secure + bank*0x1000 + port*0x40  + pin*0x8  (SCR at +0x04)
 * Per-pin: ENABLE_CONFIG 0x00 (bit0 EN, bit1 OUT), INPUT 0x08,
 * OUTPUT_CONTROL 0x0c (bit0 FLOATED), OUTPUT_VALUE 0x10.
 *
 * gpio_no encoding: main = port_index*8 + pin (port_index A=0..AG=24);
 * AON = 256 + port_index*8 + pin (AA=0..GG=5).  This matches the DT
 * TEGRA234_MAIN_GPIO(port,pin) macro (e.g. SD card detect G7 = 6*8+7 = 55).
 *
 * The shared libgpio interface (config/pull/write/read) is implemented on
 * top; pull is a no-op (Tegra pull lives in pinmux, left as firmware set it).
 * Pinmux GPIO/SFIO selection is NOT reprogrammed here to avoid disturbing
 * firmware pin configuration.
 */

#define GPIO_MAIN_VA   0x66000000ULL
#define GPIO_MAIN_SEC_VA 0x66010000ULL
#define GPIO_AON_VA    0x66020000ULL
#define GPIO_AON_SEC_VA 0x66030000ULL
#define AON_GPIO_BASE  256u

#define R_ENABLE_CONFIG  0x00
#define R_INPUT          0x08
#define R_OUTPUT_CONTROL 0x0c
#define R_OUTPUT_VALUE   0x10
#define EN_ENABLE        (1u << 0)
#define EN_OUT           (1u << 1)
#define OUT_FLOATED      (1u << 0)
#define SCR_SEC_WEN      (1u << 28)
#define SCR_SEC_REN      (1u << 27)
#define SCR_SEC_G1W      (1u << 9)
#define SCR_SEC_G1R      (1u << 1)

typedef struct {
    uint8_t bank, port, pins;
} gpio_port_t;

static const gpio_port_t _main_ports[] = {
    {0,0,8},{0,3,1},{5,1,8},{5,2,4},{5,3,8},{5,4,6},{4,0,8},{4,1,8},
    {4,2,7},{5,0,6},{3,0,8},{3,1,4},{2,0,8},{2,1,8},{2,2,8},{2,3,8},
    {2,4,6},{1,0,8},{1,1,8},{1,2,8},{0,1,8},{0,2,4},{3,3,2},{3,4,4},{3,2,8},
};
static const gpio_port_t _aon_ports[] = {
    {0,4,8},{0,5,4},{0,2,8},{0,3,3},{0,0,8},{0,1,1},
};

static volatile uint8_t* _main;
static volatile uint8_t* _main_sec;
static volatile uint8_t* _aon;
static volatile uint8_t* _aon_sec;
static int _inited;

static inline uint32_t rd(volatile uint8_t* b, uint32_t off) {
    return *(volatile uint32_t*)(b + off);
}
static inline void wr(volatile uint8_t* b, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(b + off) = v;
}

static int map(uint64_t va, uint64_t phy, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va, (ewokos_addr_t)phy,
            (ewokos_addr_t)size) == (ewokos_addr_t)va ? 0 : -1;
}

void bsp_gpio_init(void) {
    if (_inited)
        return;
    if (map(GPIO_MAIN_VA, ORIN_GPIO_MAIN_PHY, ORIN_GPIO_REG_SIZE) != 0 ||
            map(GPIO_MAIN_SEC_VA, ORIN_GPIO_MAIN_SEC_PHY, ORIN_GPIO_REG_SIZE) != 0 ||
            map(GPIO_AON_VA, ORIN_GPIO_AON_PHY, ORIN_GPIO_REG_SIZE) != 0 ||
            map(GPIO_AON_SEC_VA, ORIN_GPIO_AON_SEC_PHY, ORIN_GPIO_REG_SIZE) != 0) {
        slog("orin gpio: register map failed\n");
        return;
    }
    _main = (volatile uint8_t*)(uintptr_t)GPIO_MAIN_VA;
    _main_sec = (volatile uint8_t*)(uintptr_t)GPIO_MAIN_SEC_VA;
    _aon = (volatile uint8_t*)(uintptr_t)GPIO_AON_VA;
    _aon_sec = (volatile uint8_t*)(uintptr_t)GPIO_AON_SEC_VA;
    _inited = 1;
}

/* Resolve gpio_no to its register/secure base; returns 0 on success. */
static int resolve(int32_t no, volatile uint8_t** reg, volatile uint8_t** sec) {
    if (!_inited)
        bsp_gpio_init();
    if (!_inited)
        return -1;
    const gpio_port_t* ports;
    uint32_t nports, idx, pin;
    volatile uint8_t* base;
    volatile uint8_t* sbase;
    if (no >= (int32_t)AON_GPIO_BASE) {
        uint32_t a = (uint32_t)no - AON_GPIO_BASE;
        idx = a / 8; pin = a % 8;
        ports = _aon_ports; nports = sizeof(_aon_ports)/sizeof(_aon_ports[0]);
        base = _aon; sbase = _aon_sec;
    } else if (no >= 0) {
        idx = (uint32_t)no / 8; pin = (uint32_t)no % 8;
        ports = _main_ports; nports = sizeof(_main_ports)/sizeof(_main_ports[0]);
        base = _main; sbase = _main_sec;
    } else {
        return -1;
    }
    if (idx >= nports || pin >= ports[idx].pins)
        return -1;
    uint32_t off = ports[idx].bank * 0x1000u + ports[idx].port * 0x200u + pin * 0x20u;
    uint32_t soff = ports[idx].bank * 0x1000u + ports[idx].port * 0x40u + pin * 0x8u;
    *reg = base + off;
    *sec = sbase + soff;
    return 0;
}

static int accessible(volatile uint8_t* sec) {
    uint32_t scr = rd(sec, 0x04);
    int r_ok = !(scr & SCR_SEC_REN) || (scr & SCR_SEC_G1R);
    int w_ok = !(scr & SCR_SEC_WEN) || (scr & SCR_SEC_G1W);
    return r_ok && w_ok;
}

void bsp_gpio_config(int32_t no, int32_t sel) {
    volatile uint8_t* reg;
    volatile uint8_t* sec;
    if (resolve(no, &reg, &sec) != 0 || !accessible(sec))
        return;
    uint32_t en = rd(reg, R_ENABLE_CONFIG);
    if (sel == 0x01) {           /* GPIO_OUTPUT */
        wr(reg, R_OUTPUT_CONTROL, rd(reg, R_OUTPUT_CONTROL) & ~OUT_FLOATED);
        wr(reg, R_ENABLE_CONFIG, en | EN_ENABLE | EN_OUT);
    } else {                     /* GPIO_INPUT (and default) */
        wr(reg, R_OUTPUT_CONTROL, rd(reg, R_OUTPUT_CONTROL) | OUT_FLOATED);
        wr(reg, R_ENABLE_CONFIG, (en | EN_ENABLE) & ~EN_OUT);
    }
}

void bsp_gpio_pull(int32_t no, int32_t pull_dir) {
    (void)no; (void)pull_dir;    /* Tegra pull is in pinmux; leave as set */
}

void bsp_gpio_write(int32_t no, int32_t value) {
    volatile uint8_t* reg;
    volatile uint8_t* sec;
    if (resolve(no, &reg, &sec) != 0 || !accessible(sec))
        return;
    uint32_t v = rd(reg, R_OUTPUT_VALUE);
    wr(reg, R_OUTPUT_VALUE, value ? (v | 1u) : (v & ~1u));
    wr(reg, R_OUTPUT_CONTROL, rd(reg, R_OUTPUT_CONTROL) & ~OUT_FLOATED);
    wr(reg, R_ENABLE_CONFIG, rd(reg, R_ENABLE_CONFIG) | EN_ENABLE | EN_OUT);
}

uint8_t bsp_gpio_read(int32_t no) {
    volatile uint8_t* reg;
    volatile uint8_t* sec;
    if (resolve(no, &reg, &sec) != 0)
        return 0;
    uint32_t en = rd(reg, R_ENABLE_CONFIG);
    uint32_t v = (en & EN_OUT) ? rd(reg, R_OUTPUT_VALUE) : rd(reg, R_INPUT);
    return (uint8_t)(v & 1u);
}

#define PINMUX_VA 0x66040000ULL
static volatile uint8_t* _pinmux;

uint32_t bsp_pinmux_read(uint32_t offset) {
    if (offset >= ORIN_PINMUX_MAIN_SIZE)
        return 0xffffffffu;
    if (_pinmux == NULL) {
        if (map(PINMUX_VA, ORIN_PINMUX_MAIN_PHY, ORIN_PINMUX_MAIN_SIZE) != 0)
            return 0xffffffffu;
        _pinmux = (volatile uint8_t*)(uintptr_t)PINMUX_VA;
    }
    return rd(_pinmux, offset);
}