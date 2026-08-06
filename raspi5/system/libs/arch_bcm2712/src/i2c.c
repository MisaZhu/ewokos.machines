#include <arch/bcm2712/i2c.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/mmio.h>
#include <ewoksys/mmio.h>
#include <unistd.h>

#define I2C_BUS_COUNT           4
#define I2C_FIFO_DEPTH          32
#define I2C_TIMEOUT_US          500000

#define DW_IC_CON               0x00
#define DW_IC_TAR               0x04
#define DW_IC_DATA_CMD          0x10
#define DW_IC_SS_SCL_HCNT       0x14
#define DW_IC_SS_SCL_LCNT       0x18
#define DW_IC_FS_SCL_HCNT       0x1c
#define DW_IC_FS_SCL_LCNT       0x20
#define DW_IC_INTR_MASK         0x30
#define DW_IC_RAW_INTR_STAT     0x34
#define DW_IC_RX_TL             0x38
#define DW_IC_TX_TL             0x3c
#define DW_IC_CLR_INTR          0x40
#define DW_IC_CLR_TX_ABRT       0x54
#define DW_IC_ENABLE            0x6c
#define DW_IC_STATUS            0x70
#define DW_IC_TXFLR             0x74
#define DW_IC_RXFLR             0x78
#define DW_IC_SDA_HOLD          0x7c
#define DW_IC_TX_ABRT_SOURCE    0x80
#define DW_IC_ENABLE_STATUS     0x9c
#define DW_IC_COMP_TYPE         0xfc

#define IC_CON_MASTER           (1u << 0)
#define IC_CON_SPEED_STD        (1u << 1)
#define IC_CON_RESTART_EN       (1u << 5)
#define IC_CON_SLAVE_DISABLE    (1u << 6)
#define IC_STATUS_ACTIVITY      (1u << 0)
#define IC_INTR_TX_ABRT         (1u << 6)
#define IC_INTR_STOP_DET        (1u << 9)
#define IC_CMD_READ             (1u << 8)
#define IC_CMD_STOP             (1u << 9)
#define IC_CMD_RESTART          (1u << 10)
#define IC_COMP_TYPE_VALUE      0x44570140u

static const uint8_t i2c_pins[I2C_BUS_COUNT][2] = {
	{0, 1}, {2, 3}, {4, 5}, {6, 7}
};

static ewokos_addr_t i2c_base(int bus) {
	return _mmio_base + PI5_RP1_WIN_OFF + 0x70000 + (uint32_t)bus * 0x4000;
}

static int disable_adapter(ewokos_addr_t base) {
	for (int i = 0; i < 100; i++) {
		put32(base + DW_IC_ENABLE, 0);
		if (!(get32(base + DW_IC_ENABLE_STATUS) & 1))
			return 0;
		usleep(25);
	}
	return BCM2712_I2C_TIMEOUT;
}

int bcm2712_i2c_init(int bus) {
	if (bus < 0 || bus >= I2C_BUS_COUNT)
		return BCM2712_I2C_INVALID;

	ewokos_addr_t base = i2c_base(bus);
	if (get32(base + DW_IC_COMP_TYPE) != IC_COMP_TYPE_VALUE)
		return BCM2712_I2C_NODEV;
	if (disable_adapter(base) < 0)
		return BCM2712_I2C_TIMEOUT;

	bcm2712_gpio_init();
	bcm2712_gpio_config(i2c_pins[bus][0], GPIO_FUNC_ALTF3);
	bcm2712_gpio_config(i2c_pins[bus][1], GPIO_FUNC_ALTF3);
	bcm2712_gpio_pull(i2c_pins[bus][0], GPIO_PULL_UP);
	bcm2712_gpio_pull(i2c_pins[bus][1], GPIO_PULL_UP);

	/* RP1 feeds the DesignWare block with a fixed 200 MHz system clock. */
	put32(base + DW_IC_SS_SCL_HCNT, 857);
	put32(base + DW_IC_SS_SCL_LCNT, 999);
	put32(base + DW_IC_FS_SCL_HCNT, 179);
	put32(base + DW_IC_FS_SCL_LCNT, 319);
	put32(base + DW_IC_SDA_HOLD, get32(base + DW_IC_SDA_HOLD) | (1u << 16));
	put32(base + DW_IC_CON, IC_CON_MASTER | IC_CON_SPEED_STD |
		IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE);
	put32(base + DW_IC_TX_TL, I2C_FIFO_DEPTH / 2);
	put32(base + DW_IC_RX_TL, 0);
	put32(base + DW_IC_INTR_MASK, 0);
	(void)get32(base + DW_IC_CLR_INTR);
	return 0;
}

int bcm2712_i2c_write_read(int bus, uint8_t addr,
		const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen) {
	if (bus < 0 || bus >= I2C_BUS_COUNT || addr >= 0x80 || wlen < 0 || rlen < 0 ||
		(wlen && !wbuf) || (rlen && !rbuf) || (!wlen && !rlen))
		return BCM2712_I2C_INVALID;

	ewokos_addr_t base = i2c_base(bus);
	unsigned waited = 0;
	while (get32(base + DW_IC_STATUS) & IC_STATUS_ACTIVITY) {
		if (waited++ >= I2C_TIMEOUT_US / 10)
			return BCM2712_I2C_BUSY;
		usleep(10);
	}

	if (disable_adapter(base) < 0)
		return BCM2712_I2C_TIMEOUT;
	put32(base + DW_IC_TAR, addr);
	put32(base + DW_IC_INTR_MASK, 0);
	(void)get32(base + DW_IC_CLR_INTR);
	put32(base + DW_IC_ENABLE, 1);

	int wi = 0, queued_reads = 0, ri = 0;
	waited = 0;
	while (ri < rlen || wi < wlen || queued_reads < rlen) {
		uint32_t raw = get32(base + DW_IC_RAW_INTR_STAT);
		if (raw & IC_INTR_TX_ABRT) {
			(void)get32(base + DW_IC_TX_ABRT_SOURCE);
			(void)get32(base + DW_IC_CLR_TX_ABRT);
			disable_adapter(base);
			return BCM2712_I2C_NACK;
		}

		while (ri < rlen && get32(base + DW_IC_RXFLR))
			rbuf[ri++] = (uint8_t)get32(base + DW_IC_DATA_CMD);

		while (get32(base + DW_IC_TXFLR) < I2C_FIFO_DEPTH) {
			uint32_t cmd;
			if (wi < wlen) {
				cmd = wbuf[wi++];
				if (wi == wlen && rlen == 0)
					cmd |= IC_CMD_STOP;
			} else if (queued_reads < rlen && queued_reads - ri < I2C_FIFO_DEPTH) {
				cmd = IC_CMD_READ;
				if (queued_reads == 0 && wlen)
					cmd |= IC_CMD_RESTART;
				if (++queued_reads == rlen)
					cmd |= IC_CMD_STOP;
			} else {
				break;
			}
			put32(base + DW_IC_DATA_CMD, cmd);
		}

		if (++waited >= I2C_TIMEOUT_US) {
			disable_adapter(base);
			return BCM2712_I2C_TIMEOUT;
		}
		usleep(1);
	}

	while (!(get32(base + DW_IC_RAW_INTR_STAT) & (IC_INTR_STOP_DET | IC_INTR_TX_ABRT))) {
		if (++waited >= I2C_TIMEOUT_US) {
			disable_adapter(base);
			return BCM2712_I2C_TIMEOUT;
		}
		usleep(1);
	}
	if (get32(base + DW_IC_RAW_INTR_STAT) & IC_INTR_TX_ABRT) {
		(void)get32(base + DW_IC_CLR_TX_ABRT);
		disable_adapter(base);
		return BCM2712_I2C_NACK;
	}
	(void)get32(base + DW_IC_CLR_INTR);
	disable_adapter(base);
	return wlen + rlen;
}

int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len) {
	int ret = bcm2712_i2c_write_read(bus, addr, 0, 0, buf, len);
	return ret < 0 ? ret : len;
}

int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len) {
	int ret = bcm2712_i2c_write_read(bus, addr, buf, len, 0, 0);
	return ret < 0 ? ret : len;
}
