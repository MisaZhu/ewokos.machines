/*
 * RP1 I2C master driver for BCM2712 (Raspberry Pi 5).
 *
 * The seven controllers are Synopsys DW_apb_i2c instances inside the RP1
 * southbridge (rp1.dtsi i2c@70000 .. i2c@88000, "snps,designware-i2c"),
 * fed by RP1_CLK_SYS at 200 MHz. This is a complete break from the BCM283x
 * driver, which bit-banged the bus over two GPIOs: here the controller
 * runs the wire protocol and we only feed the TX/RX FIFOs, polled.
 *
 * Register names, the SCL timing formulas and the FIFO batching follow the
 * linux driver (drivers/i2c/busses/i2c-designware-*). No interrupts are
 * used: INTR_MASK stays zero and completion/NAK are read back from
 * IC_STATUS and the raw TX_ABRT bit.
 */
#include <arch/bcm2712/i2c.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <unistd.h>

#define RP1_I2C_NUM         7
#define RP1_I2C_OFF(bus)    (PI5_RP1_WIN_OFF + 0x70000 + (bus) * 0x4000)

/* DW_apb_i2c register file */
#define IC_CON              0x00
#define IC_TAR              0x04
#define IC_DATA_CMD         0x10
#define IC_SS_SCL_HCNT      0x14
#define IC_SS_SCL_LCNT      0x18
#define IC_FS_SCL_HCNT      0x1c
#define IC_FS_SCL_LCNT      0x20
#define IC_INTR_MASK        0x30
#define IC_RAW_INTR_STAT    0x34
#define IC_RX_TL            0x38
#define IC_TX_TL            0x3c
#define IC_CLR_INTR         0x40
#define IC_CLR_TX_ABRT      0x54
#define IC_ENABLE           0x6c
#define IC_STATUS           0x70
#define IC_SDA_HOLD         0x7c
#define IC_TX_ABRT_SOURCE   0x80
#define IC_ENABLE_STATUS    0x9c
#define IC_COMP_PARAM_1     0xf4
#define IC_COMP_TYPE        0xfc
#define IC_COMP_TYPE_VALUE  0x44570140

#define IC_CON_MASTER           (1 << 0)
#define IC_CON_SPEED_STD        (1 << 1)
#define IC_CON_SPEED_FAST       (2 << 1)
#define IC_CON_SPEED_MASK       (3 << 1)
#define IC_CON_RESTART_EN       (1 << 5)
#define IC_CON_SLAVE_DISABLE    (1 << 6)
/* hold the bus instead of overflowing when the RX FIFO fills up; reads back
 * as 0 if the instance was synthesized without it, which is harmless */
#define IC_CON_RX_FULL_HLD      (1 << 9)

#define IC_DATA_CMD_READ        (1 << 8)
#define IC_DATA_CMD_STOP        (1 << 9)

#define IC_RAW_TX_ABRT          (1 << 6)
#define IC_RAW_MST_ON_HOLD      (1 << 13)

#define IC_ENABLE_EN            (1 << 0)
#define IC_ENABLE_ABORT         (1 << 1)

#define IC_STATUS_ACTIVITY      (1 << 0)
#define IC_STATUS_TFNF          (1 << 1)
#define IC_STATUS_TFE           (1 << 2)
#define IC_STATUS_RFNE          (1 << 3)

#define IC_ENABLE_STATUS_EN     (1 << 0)

/*
 * SCL counts in clk_sys cycles, from linux i2c_dw_scl_hcnt()/_lcnt() with
 * ic_clk = 200000 kHz. rp1.dtsi declares i2c-scl-falling-time-ns = 100 and
 * leaves i2c-sda-falling-time-ns unset, so the designware driver uses
 * tf = 300ns for HCNT and tf = 100ns for LCNT, giving
 *   standard (tHD;STA/tHIGH 4.0us, tLOW 4.7us): 857 / 959
 *   fast     (tHIGH 0.6us, tLOW 1.3us):         177 / 279
 * which is what mainline programs for this controller. The counts follow
 * the DW default (-3) strategy that keeps tHD;STA legal rather than the
 * "ideal" (-8) one, so the resulting SCL lands near, and never far below,
 * the nominal 100/400 kHz.
 */
#define I2C_CLK_KHZ         200000
#define I2C_SDA_FALL_NS     300
#define I2C_SCL_FALL_NS     100
#define SCL_CNT(ns, tf)     ((I2C_CLK_KHZ * ((ns) + (tf)) + 500000) / 1000000)
#define SCL_HCNT(thigh_ns)  (SCL_CNT(thigh_ns, I2C_SDA_FALL_NS) - 3)
#define SCL_LCNT(tlow_ns)   (SCL_CNT(tlow_ns, I2C_SCL_FALL_NS) - 1)
#define SDA_HOLD_CNT        (I2C_CLK_KHZ * 300 / 1000000)   /* 300ns */

/*
 * No-progress poll budget. One iteration is a handful of device-memory
 * reads (~0.5us), so this is a few tens of milliseconds: long enough for a
 * slave that stretches the clock, short enough that a dead bus cannot wedge
 * the caller's IPC loop for seconds.
 */
#define I2C_POLL_MAX        100000
/* IC_ENABLE_STATUS follows IC_ENABLE within a few clk_sys cycles */
#define I2C_ENABLE_POLL_MAX 100

static uint8_t  _i2c_ready[RP1_I2C_NUM];
/* IC_CON template per bus, holds the selected speed bits */
static uint32_t _i2c_con[RP1_I2C_NUM];
/* RX FIFO depth from IC_COMP_PARAM_1; 1 if the instance does not report it */
static uint16_t _i2c_rx_depth[RP1_I2C_NUM];

/*
 * Default header pinmux. The Pi5 alt-function table puts i2c on a3 for the
 * first eight header pins: SDA0/SCL0 on GPIO0/1, SDA1/SCL1 on GPIO2/3
 * (the header default, with the board's 1.8k pull-ups), SDA2/SCL2 on
 * GPIO4/5, SDA3/SCL3 on GPIO6/7.
 */
static const uint8_t _i2c_pins[4][2] = {
	{0, 1}, {2, 3}, {4, 5}, {6, 7}
};

static inline ewokos_addr_t i2c_base(int bus) {
	return _mmio_base + RP1_I2C_OFF(bus);
}

static inline int i2c_comp_type_ok(ewokos_addr_t base) {
	return get32(base + IC_COMP_TYPE) == IC_COMP_TYPE_VALUE;
}

static int i2c_set_enable(ewokos_addr_t base, uint32_t en) {
	put32(base + IC_ENABLE, en);
	for (uint32_t n = 0; n < I2C_ENABLE_POLL_MAX; n++) {
		if ((get32(base + IC_ENABLE_STATUS) & IC_ENABLE_STATUS_EN) == en)
			return 0;
		usleep(25);
	}
	return -1;
}

/*
 * Clear a pending abort and park the controller; disabling flushes the FIFOs.
 *
 * A poll timeout typically means the TX FIFO ran dry mid transaction, and
 * with EMPTYFIFO_HOLD_MASTER_EN the master is then holding SCL low. In that
 * state the hardware ignores a write clearing IC_ENABLE, so ask for an abort
 * first and let the controller emit a STOP and release the bus, the way
 * linux __i2c_dw_disable() does. Without this the bus would stay wedged for
 * every later transfer.
 */
static void i2c_recover(ewokos_addr_t base) {
	if (get32(base + IC_RAW_INTR_STAT) & IC_RAW_MST_ON_HOLD) {
		put32(base + IC_ENABLE, IC_ENABLE_EN | IC_ENABLE_ABORT);
		for (uint32_t n = 0; n < I2C_POLL_MAX; n++) {
			if ((get32(base + IC_ENABLE) & IC_ENABLE_ABORT) == 0)
				break;
		}
	}
	(void)get32(base + IC_CLR_TX_ABRT);
	(void)get32(base + IC_CLR_INTR);
	i2c_set_enable(base, 0);
}

int bcm2712_i2c_init(int bus) {
	if (bus < 0 || bus >= RP1_I2C_NUM)
		return BCM2712_I2C_ERR_INVALID;

	/* same window setup as the other RP1 users (uartd, bsp_sd, spi) */
	sys_info_t sysinfo;
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	_mmio_base = sysinfo.mmio.v_base;
	ewokos_addr_t main_mapped = syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)sysinfo.mmio.v_base,
			(ewokos_addr_t)sysinfo.mmio.phy_base,
			(ewokos_addr_t)sysinfo.mmio.size);
	if (main_mapped != sysinfo.mmio.v_base) {
		klog("i2c-rp1: main map failed got=%p expected=%p\n",
			(void *)main_mapped, (void *)sysinfo.mmio.v_base);
		return BCM2712_I2C_ERR_MAIN_MAP;
	}
	ewokos_addr_t rp1_vbase = _mmio_base + PI5_RP1_WIN_OFF;
	ewokos_addr_t rp1_mapped = syscall3(SYS_MEM_MAP,
			_mmio_base + PI5_RP1_WIN_OFF,
			PI5_RP1_PHY,
			PI5_RP1_WIN_SIZE);
	if (rp1_mapped != rp1_vbase) {
		klog("i2c-rp1: RP1 map failed got=%p expected=%p\n",
			(void *)rp1_mapped, (void *)rp1_vbase);
		return BCM2712_I2C_ERR_RP1_MAP;
	}

	ewokos_addr_t base = i2c_base(bus);
	if (!i2c_comp_type_ok(base)) {
		int rp1_ret = bcm2712_rp1_init();
		if (rp1_ret != 0)
			return rp1_ret - 10;
	}

	if (bus < 4) {
		bcm2712_gpio_init();
		for (int i = 0; i < 2; i++) {
			uint32_t pin = _i2c_pins[bus][i];
			bcm2712_gpio_pull(pin, GPIO_PULL_UP);
			bcm2712_gpio_config(pin, GPIO_FUNC_ALTF3);
		}
	}

	uint32_t comp_type = get32(base + IC_COMP_TYPE);
	if (comp_type != IC_COMP_TYPE_VALUE) {
		klog("i2c-rp1: bus=%d base=%p bad COMP_TYPE=%08x expected=%08x\n",
			bus, (void *)base, comp_type, IC_COMP_TYPE_VALUE);
		return BCM2712_I2C_ERR_COMP_TYPE;
	}
	if (i2c_set_enable(base, 0) != 0) {
		klog("i2c-rp1: bus=%d disable timeout enable=%08x enable_status=%08x status=%08x\n",
			bus, get32(base + IC_ENABLE), get32(base + IC_ENABLE_STATUS),
			get32(base + IC_STATUS));
		return BCM2712_I2C_ERR_DISABLE;
	}

	put32(base + IC_INTR_MASK, 0);
	put32(base + IC_SS_SCL_HCNT, SCL_HCNT(4000));
	put32(base + IC_SS_SCL_LCNT, SCL_LCNT(4700));
	put32(base + IC_FS_SCL_HCNT, SCL_HCNT(600));
	put32(base + IC_FS_SCL_LCNT, SCL_LCNT(1300));
	put32(base + IC_SDA_HOLD, SDA_HOLD_CNT);
	/* polled, so the FIFO thresholds only need to be harmless */
	put32(base + IC_RX_TL, 0);
	put32(base + IC_TX_TL, 0);

	/*
	 * Encoded FIFO depths, same fields linux reads. A controller
	 * synthesized without ADD_ENCODED_PARAMS reads 0 here, which yields
	 * depth 1 and degrades the transfer loop to one byte in flight
	 * instead of overflowing anything.
	 */
	uint32_t param = get32(base + IC_COMP_PARAM_1);
	_i2c_rx_depth[bus] = ((param >> 8) & 0xff) + 1;

	_i2c_con[bus] = IC_CON_MASTER | IC_CON_SLAVE_DISABLE |
			IC_CON_RESTART_EN | IC_CON_RX_FULL_HLD |
			IC_CON_SPEED_STD;
	put32(base + IC_CON, _i2c_con[bus]);

	_i2c_ready[bus] = 1;
	return 0;
}

int bcm2712_i2c_set_speed(int bus, uint32_t hz) {
	if (bus < 0 || bus >= RP1_I2C_NUM)
		return -1;
	if (!_i2c_ready[bus] && bcm2712_i2c_init(bus) != 0)
		return -1;

	ewokos_addr_t base = i2c_base(bus);
	if (i2c_set_enable(base, 0) != 0)
		return -1;

	_i2c_con[bus] &= ~IC_CON_SPEED_MASK;
	_i2c_con[bus] |= (hz <= 100000) ? IC_CON_SPEED_STD : IC_CON_SPEED_FAST;
	put32(base + IC_CON, _i2c_con[bus]);
	return 0;
}

/*
 * One bus transaction: write wlen bytes, then (if rlen > 0) read rlen bytes.
 * With IC_RESTART_EN the controller inserts the repeated start by itself
 * when the direction flips; the explicit STOP flag on the last command ends
 * the transfer.
 *
 * Commands are batched up to the FIFO depths (linux i2c_dw_xfer_msg does the
 * same) rather than one byte at a time, so a transfer that fits in the FIFO
 * never lets the TX FIFO run dry mid-transaction. Read commands are only
 * queued once every write byte is queued, otherwise the direction would
 * flip in the wrong place; outstanding read commands are capped by the RX
 * depth so the RX FIFO cannot overflow.
 */
static int i2c_xfer(int bus, uint8_t addr, const uint8_t *wbuf, int wlen,
		uint8_t *rbuf, int rlen) {
	if (bus < 0 || bus >= RP1_I2C_NUM)
		return -1;
	if (!_i2c_ready[bus] && bcm2712_i2c_init(bus) != 0)
		return -1;
	if (addr > 0x7f)
		return -1;
	if (wlen < 0 || rlen < 0 || (wlen + rlen) == 0)
		return -1;
	if ((wlen > 0 && wbuf == (const uint8_t*)0) ||
			(rlen > 0 && rbuf == (uint8_t*)0))
		return -1;

	ewokos_addr_t base = i2c_base(bus);
	/* a previous caller may have been killed mid transaction, so try to
	 * free the bus once before giving up on it */
	if (i2c_set_enable(base, 0) != 0) {
		i2c_recover(base);
		if (i2c_set_enable(base, 0) != 0)
			return -1;
	}
	put32(base + IC_CON, _i2c_con[bus]);
	put32(base + IC_TAR, addr);
	(void)get32(base + IC_CLR_INTR);
	if (i2c_set_enable(base, 1) != 0)
		return -1;

	int w = 0;         /* write bytes queued */
	int rq = 0;        /* read commands queued */
	int r = 0;         /* read bytes taken out of the RX FIFO */
	uint32_t idle = 0;

	while (w < wlen || r < rlen) {
		int progress = 0;

		while (w < wlen &&
				(get32(base + IC_STATUS) & IC_STATUS_TFNF) != 0) {
			uint32_t cmd = wbuf[w];
			if (w == wlen - 1 && rlen == 0)
				cmd |= IC_DATA_CMD_STOP;
			put32(base + IC_DATA_CMD, cmd);
			w++;
			progress = 1;
		}

		if (w == wlen) {
			while (rq < rlen && (rq - r) < _i2c_rx_depth[bus] &&
					(get32(base + IC_STATUS) & IC_STATUS_TFNF) != 0) {
				uint32_t cmd = IC_DATA_CMD_READ;
				if (rq == rlen - 1)
					cmd |= IC_DATA_CMD_STOP;
				put32(base + IC_DATA_CMD, cmd);
				rq++;
				progress = 1;
			}
		}

		while (r < rq &&
				(get32(base + IC_STATUS) & IC_STATUS_RFNE) != 0) {
			rbuf[r] = get32(base + IC_DATA_CMD) & 0xff;
			r++;
			progress = 1;
		}

		if (get32(base + IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT)
			goto abort;

		if (progress)
			idle = 0;
		else if (++idle > I2C_POLL_MAX)
			goto abort;
	}

	/*
	 * Drain the TX FIFO and wait for the stop to reach the wire; a NAK on
	 * the last written byte only shows up as TX_ABRT here.
	 */
	for (idle = 0; ; idle++) {
		if (get32(base + IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT)
			goto abort;
		uint32_t st = get32(base + IC_STATUS);
		if ((st & IC_STATUS_TFE) != 0 && (st & IC_STATUS_ACTIVITY) == 0)
			break;
		if (idle > I2C_POLL_MAX)
			goto abort;
	}

	i2c_set_enable(base, 0);
	return 0;

abort:
	i2c_recover(base);
	return -1;
}

int bcm2712_i2c_write(int bus, uint8_t addr, const uint8_t *buf, int len) {
	return i2c_xfer(bus, addr, buf, len, (uint8_t*)0, 0);
}

int bcm2712_i2c_read(int bus, uint8_t addr, uint8_t *buf, int len) {
	return i2c_xfer(bus, addr, (const uint8_t*)0, 0, buf, len);
}

int bcm2712_i2c_write_read(int bus, uint8_t addr,
		const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen) {
	return i2c_xfer(bus, addr, wbuf, wlen, rbuf, rlen);
}

int bcm2712_i2c_putb(int bus, uint8_t addr, uint8_t reg, uint8_t data) {
	uint8_t buf[2] = { reg, data };
	return i2c_xfer(bus, addr, buf, 2, (uint8_t*)0, 0);
}

int bcm2712_i2c_getb(int bus, uint8_t addr, uint8_t reg) {
	uint8_t data = 0;
	if (i2c_xfer(bus, addr, &reg, 1, &data, 1) != 0)
		return -1;
	return data;
}
