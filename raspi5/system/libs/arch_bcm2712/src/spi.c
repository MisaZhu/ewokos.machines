/*
 * RP1 SPI master driver for BCM2712 (Raspberry Pi 5).
 *
 * The buses are Synopsys DW_apb_ssi instances inside the RP1 southbridge
 * (rp1.dtsi spi@50000 .. spi@6c000 plus spi8 @ 0x4c000, "snps,dw-apb-ssi",
 * clocked by RP1_CLK_SYS at 200 MHz). Like the i2c rewrite this replaces
 * BCM283x era code entirely: nothing of the 0x204000 SPI0 block exists here.
 *
 * "snps,dw-apb-ssi" is the classic DW_apb_ssi programming model, not the
 * newer DWC_ssi one, so CTRLR0 keeps DFS at 3:0 and TMOD at 9:8.
 *
 * Register names and the FIFO depth probe follow the linux driver
 * (drivers/spi/spi-dw*). Polled, mode 0, 8-bit frames. Chip select is
 * driven in software over RIO GPIO (GPIO8/CE0, GPIO7/CE1): the controller's
 * native SS drops on any TX FIFO underrun, which is why the official device
 * tree uses cs-gpios here too. SER is still set, because with no slave
 * enabled the state machine never clocks a frame out.
 */
#include <arch/bcm2712/spi.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>


#define RP1_SPI_NUM         9

/* DW_apb_ssi register file */
#define SSI_CTRLR0          0x00
#define SSI_SSIENR          0x08
#define SSI_SER             0x10
#define SSI_BAUDR           0x14
#define SSI_TXFTLR          0x18
#define SSI_RXFTLR          0x1c
#define SSI_SR              0x28
#define SSI_IMR             0x2c
#define SSI_ICR             0x48
#define SSI_DR              0x60

/* CTRLR0: classic DW_apb_ssi DFS stores nbits - 1 in bits 3:0 */
#define SSI_CTRLR0_DFS_8        0x7
#define SSI_CTRLR0_DFS_16       0xf
#define SSI_CTRLR0_SCPH         (1 << 6)
#define SSI_CTRLR0_SCPOL        (1 << 7)
#define SSI_CTRLR0_TMOD_TR      (0 << 8)
#define SSI_CTRLR0_TMOD_TO      (1 << 8)

#define SSI_SR_BUSY             (1 << 0)
#define SSI_SR_TFNF             (1 << 1)
#define SSI_SR_TFE              (1 << 2)
#define SSI_SR_RFNE             (1 << 3)

#define SPI_CLK_KHZ         200000
#define SPI_DIV_DEFAULT     20      /* 200MHz / 20 = 10MHz */
#define SPI_DIV_MAX         0xfffe  /* slowest, ~3kHz */

/*
 * No-progress poll budget, reset whenever a frame moves. One iteration is a
 * couple of device-memory reads, so this is tens of milliseconds: enough for
 * the slowest divider, short enough that a wedged bus does not stall the
 * caller's IPC loop for seconds. A long transfer is not penalised because
 * the counter only counts iterations that moved nothing.
 */
#define SPI_POLL_MAX        100000

/* header spi0 pins, funcsel a0 for the bus lines, RIO for the CS pins */
#define SPI0_SCLK_PIN       11
#define SPI0_MOSI_PIN       10
#define SPI0_MISO_PIN       9
#define SPI0_CE0_PIN        8
#define SPI0_CE1_PIN        7

static uint8_t  _spi_ready[RP1_SPI_NUM];
/* probed FIFO depth, also the cap on frames in flight */
static uint16_t _spi_fifo_len[RP1_SPI_NUM];
static uint32_t _spi_ctrlr0[RP1_SPI_NUM];
static uint32_t _spi0_cs_pin = SPI0_CE0_PIN;

static inline ewokos_addr_t spi_base(int bus) {
    uint32_t off = (bus == 8) ? 0x4c000 : 0x50000 + (uint32_t)bus * 0x4000;
    return _mmio_base + PI5_RP1_WIN_OFF + off;
}

/*
 * DW_apb_ssi does not report its FIFO depth in a register, so find it the
 * way linux dw_spi_hw_init() does: TXFTLR only accepts values below the
 * depth. Must run with the controller disabled.
 */
static uint32_t spi_probe_fifo_len(ewokos_addr_t base) {
    uint32_t len;
    for (len = 1; len < 256; len++) {
        put32(base + SSI_TXFTLR, len);
        if (get32(base + SSI_TXFTLR) != len)
            break;
    }
    put32(base + SSI_TXFTLR, 0);
    return (len == 1) ? 1 : len;
}

/*
 * Disabling the controller clears both FIFOs, which is the only way to get
 * back in sync after a transfer gave up half way: leftover RX frames would
 * otherwise shift every following transfer by that many bytes.
 */
static void spi_reset(ewokos_addr_t base) {
    put32(base + SSI_SSIENR, 0);
    (void)get32(base + SSI_ICR);
    put32(base + SSI_SSIENR, 1);
}

static void spi_set_ctrlr0(int bus, ewokos_addr_t base, uint32_t ctrlr0) {
    if (_spi_ctrlr0[bus] == ctrlr0)
        return;

    put32(base + SSI_SSIENR, 0);
    put32(base + SSI_CTRLR0, ctrlr0);
    (void)get32(base + SSI_ICR);
    put32(base + SSI_SSIENR, 1);
    _spi_ctrlr0[bus] = ctrlr0;
}

int bcm2712_spi_init(int bus) {
    if (bus < 0 || bus >= RP1_SPI_NUM)
        return -1;

    /* same window setup as the other RP1 users (uartd, bsp_sd, i2c) */
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

    /*
     * The RP1 register window only reliably decodes after the PCIe2 link is
     * trained and the RP1 BARs are enabled. gpio.c can touch IO_BANK after the
     * firmware leaves RP1 up, but SPI/I2C/USB have all seen boards/boot flows
     * where peripheral blocks still return a bus fault or the AXI error pattern
     * until bcm2712_rp1_init() confirms the link/BAR state. It is idempotent.
     */
    if (bcm2712_rp1_init() != 0)
        return -1;

    if (bus == 0) {
        bcm2712_gpio_init();
        bcm2712_gpio_config(SPI0_SCLK_PIN, GPIO_FUNC_ALTF0);
        bcm2712_gpio_config(SPI0_MOSI_PIN, GPIO_FUNC_ALTF0);
        bcm2712_gpio_config(SPI0_MISO_PIN, GPIO_FUNC_ALTF0);
        /*
         * Software CS. The idle level is written before the pin is
         * turned into an output, otherwise it would briefly drive
         * whatever RIO_OUT happened to hold and could assert CS.
         */
        bcm2712_gpio_write(SPI0_CE0_PIN, true);
        bcm2712_gpio_write(SPI0_CE1_PIN, true);
        bcm2712_gpio_config(SPI0_CE0_PIN, GPIO_FUNC_OUTPUT);
        bcm2712_gpio_config(SPI0_CE1_PIN, GPIO_FUNC_OUTPUT);
    }

    ewokos_addr_t base = spi_base(bus);
    _spi_ctrlr0[bus] = SSI_CTRLR0_DFS_8 | SSI_CTRLR0_TMOD_TR;
    put32(base + SSI_SSIENR, 0);
    put32(base + SSI_IMR, 0);
    put32(base + SSI_CTRLR0, _spi_ctrlr0[bus]);
    put32(base + SSI_BAUDR, SPI_DIV_DEFAULT);
    _spi_fifo_len[bus] = spi_probe_fifo_len(base);
    put32(base + SSI_RXFTLR, 0);
    /* internal SS0 only clocks the state machine; the pin is RIO-muxed */
    put32(base + SSI_SER, 1);
    put32(base + SSI_SSIENR, 1);

    _spi_ready[bus] = 1;
    return 0;
}

int bcm2712_spi_set_div(int bus, uint32_t div) {
    if (bus < 0 || bus >= RP1_SPI_NUM)
        return -1;
    /*
     * The shared panel/touch drivers call set_div() and select() without
     * an init() of their own (each runs in its own process), so bring the
     * bus up on demand instead of silently doing nothing.
     */
    if (!_spi_ready[bus] && bcm2712_spi_init(bus) != 0)
        return -1;

    /* BAUDR: sclk = 200MHz / div, div even, minimum 2. bcm283x read 0 as
     * 65536, i.e. the slowest clock, so keep that meaning here. */
    if (div == 0)
        div = SPI_DIV_MAX;
    if (div < 2)
        div = 2;
    div &= ~1u;
    if (div > SPI_DIV_MAX)
        div = SPI_DIV_MAX;

    ewokos_addr_t base = spi_base(bus);
    put32(base + SSI_SSIENR, 0);
    put32(base + SSI_BAUDR, div);
    put32(base + SSI_SSIENR, 1);
    return 0;
}

int bcm2712_spi_select(int bus, uint32_t which) {
    if (bus != 0)
        return -1;
    if (!_spi_ready[bus] && bcm2712_spi_init(bus) != 0)
        return -1;

    _spi0_cs_pin = (which == SPI_SELECT_1) ? SPI0_CE1_PIN : SPI0_CE0_PIN;
    return 0;
}

int bcm2712_spi_activate(int bus, uint32_t enable) {
    if (bus != 0)
        return -1;
    if (!_spi_ready[bus] && bcm2712_spi_init(bus) != 0)
        return -1;

    /* active low; the transfer path drains BUSY before returning, so
     * releasing CS here never chops a frame short */
    bcm2712_gpio_write(_spi0_cs_pin, enable == 0);
    return 0;
}

int bcm2712_spi_transfer(int bus, const void *tx, void *rx, uint32_t len) {
    if (bus < 0 || bus >= RP1_SPI_NUM)
        return -1;
    if (!_spi_ready[bus] && bcm2712_spi_init(bus) != 0)
        return -1;
    if (len == 0)
        return 0;

    ewokos_addr_t base = spi_base(bus);
    const uint8_t *txp = (const uint8_t*)tx;
    uint8_t *rxp = (uint8_t*)rx;
    uint32_t tx_n = 0, rx_n = 0, idle = 0;
    uint32_t ctrlr0 = SSI_CTRLR0_DFS_8 | SSI_CTRLR0_TMOD_TR;
    uint32_t inflight_max = _spi_fifo_len[bus];

    if (rxp == (uint8_t*)0)
        ctrlr0 = SSI_CTRLR0_DFS_8 | SSI_CTRLR0_TMOD_TO;
    spi_set_ctrlr0(bus, base, ctrlr0);

    /* start from a known state: no stale RX frames, no stale error bits */
    while ((get32(base + SSI_SR) & SSI_SR_RFNE) != 0)
        (void)get32(base + SSI_DR);
    (void)get32(base + SSI_ICR);

    if (rxp == (uint8_t*)0) {
        while (tx_n < len) {
            int progress = 0;

            while (tx_n < len &&
                    (get32(base + SSI_SR) & SSI_SR_TFNF) != 0) {
                put32(base + SSI_DR, txp ? txp[tx_n] : 0);
                tx_n++;
                progress = 1;
            }

            if (progress)
                idle = 0;
            else if (++idle > SPI_POLL_MAX) {
                spi_reset(base);
                return -1;
            }
        }
    } else {
        /*
         * TMOD_TR produces one RX frame for every byte clocked out. Capping
         * the frames in flight at the FIFO depth keeps the RX FIFO from
         * overflowing at worst exactly full.
         */
        while (rx_n < len) {
            int progress = 0;

            while (tx_n < len && (tx_n - rx_n) < inflight_max &&
                    (get32(base + SSI_SR) & SSI_SR_TFNF) != 0) {
                put32(base + SSI_DR, txp ? txp[tx_n] : 0);
                tx_n++;
                progress = 1;
            }
            while (rx_n < len &&
                    (get32(base + SSI_SR) & SSI_SR_RFNE) != 0) {
                rxp[rx_n] = get32(base + SSI_DR) & 0xff;
                rx_n++;
                progress = 1;
            }

            if (progress)
                idle = 0;
            else if (++idle > SPI_POLL_MAX) {
                spi_reset(base);
                return -1;
            }
        }
    }

    /*
     * Wait for the last frame to leave the shift register, so a caller
     * dropping CS right after cannot cut it off.
     */
    for (idle = 0; ; idle++) {
        uint32_t sr = get32(base + SSI_SR);
        if ((sr & SSI_SR_TFE) != 0 && (sr & SSI_SR_BUSY) == 0)
            break;
        if (idle > SPI_POLL_MAX) {
            spi_reset(base);
            return -1;
        }
    }
    return 0;
}

int bcm2712_spi_write16(int bus, const uint16_t *tx, uint32_t count) {
    if (bus < 0 || bus >= RP1_SPI_NUM)
        return -1;
    if (!_spi_ready[bus] && bcm2712_spi_init(bus) != 0)
        return -1;
    if (count == 0)
        return 0;

    ewokos_addr_t base = spi_base(bus);
    uint32_t tx_n = 0;
    uint32_t idle = 0;
    uint32_t ctrlr0 = SSI_CTRLR0_DFS_16 | SSI_CTRLR0_TMOD_TO;

    spi_set_ctrlr0(bus, base, ctrlr0);

    while ((get32(base + SSI_SR) & SSI_SR_RFNE) != 0)
        (void)get32(base + SSI_DR);
    (void)get32(base + SSI_ICR);

    while (tx_n < count) {
        int progress = 0;

        while (tx_n < count &&
                (get32(base + SSI_SR) & SSI_SR_TFNF) != 0) {
            put32(base + SSI_DR, tx[tx_n]);
            tx_n++;
            progress = 1;
        }

        if (progress)
            idle = 0;
        else if (++idle > SPI_POLL_MAX) {
            spi_reset(base);
            return -1;
        }
    }

    for (idle = 0; ; idle++) {
        uint32_t sr = get32(base + SSI_SR);
        if ((sr & SSI_SR_TFE) != 0 && (sr & SSI_SR_BUSY) == 0)
            break;
        if (idle > SPI_POLL_MAX) {
            spi_reset(base);
            return -1;
        }
    }
    return 0;
}
