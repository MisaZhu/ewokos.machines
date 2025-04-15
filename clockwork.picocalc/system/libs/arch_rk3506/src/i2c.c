#include <stdbool.h>
#include <ewoksys/mmio.h>

#define debug {}//printf

static uint32_t  I2C_BASE;

#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

/* i2c timerout */
#define I2C_TIMEOUT_MS      100
#define I2C_RETRY_COUNT     3

/* rk i2c fifo max transfer bytes */
#define RK_I2C_FIFO_SIZE    32

#define writel(v,a)     (*(volatile uint32_t *)(a) = (v))
#define readl(a)    (*(volatile uint32_t *)(a))
#define writew(v,a)     (*(volatile uint16_t *)(a) = (v))
#define readw(a)    (*(volatile uint16_t *)(a))
#define writeb(v,a)     (*(volatile uint8_t *)(a) = (v))
#define readb(a)    (*(volatile uint8_t *)(a))

/* Control register */
#define I2C_CON_EN      (1 << 0)
#define I2C_CON_MOD(mod)    ((mod) << 1)
#define I2C_MODE_TX     0x00
#define I2C_MODE_TRX        0x01
#define I2C_MODE_RX     0x02
#define I2C_MODE_RRX        0x03
#define I2C_CON_MASK        (3 << 1)

#define I2C_CON_START       (1 << 3)
#define I2C_CON_STOP        (1 << 4)
#define I2C_CON_LASTACK     (1 << 5)
#define I2C_CON_ACTACK      (1 << 6)
#define I2C_CON_TUNING_MASK (0xff << 8)
#define I2C_CON_SDA_CFG(cfg)    ((cfg) << 8)
#define I2C_CON_STA_CFG(cfg)    ((cfg) << 12)
#define I2C_CON_STO_CFG(cfg)    ((cfg) << 14)
#define I2C_CON_VERSION			(0xFF0000)
#define I2C_CON_VERSION_SHIFT   16

/* Clock dividor register */
#define I2C_CLK_DIV_HIGH_SHIFT  16
#define I2C_CLKDIV_VAL(divl, divh) \
    (((divl) & 0xffff) | (((divh) << 16) & 0xffff0000))

/* the slave address accessed  for master rx mode */
#define I2C_MRXADDR_SET(vld, addr)  (((vld) << 24) | (addr))

/* the slave register address accessed  for master rx mode */
#define I2C_MRXRADDR_SET(vld, raddr)    (((vld) << 24) | (raddr))

/* interrupt enable register */
#define I2C_BTFIEN      (1 << 0)
#define I2C_BRFIEN      (1 << 1)
#define I2C_MBTFIEN     (1 << 2)
#define I2C_MBRFIEN     (1 << 3)
#define I2C_STARTIEN        (1 << 4)
#define I2C_STOPIEN     (1 << 5)
#define I2C_NAKRCVIEN       (1 << 6)

/* interrupt pending register */
#define I2C_BTFIPD              (1 << 0)
#define I2C_BRFIPD              (1 << 1)
#define I2C_MBTFIPD             (1 << 2)
#define I2C_MBRFIPD             (1 << 3)
#define I2C_STARTIPD            (1 << 4)
#define I2C_STOPIPD             (1 << 5)
#define I2C_NAKRCVIPD           (1 << 6)
#define I2C_IPD_ALL_CLEAN       0x7f

typedef  volatile uint32_t u32;
typedef  uint16_t u16;
typedef  uint8_t u8;
typedef  uint8_t uchar;
typedef  unsigned long ulong;
typedef  unsigned int uint;

struct i2c_regs {
    u32 con;
    u32 clkdiv;
    u32 mrxaddr;
    u32 mrxraddr;
    u32 mtxcnt;
    u32 mrxcnt;
    u32 ien;
    u32 ipd;
    u32 fcnt;
    u32 reserved0[0x37];
    u32 txdata[8];
    u32 reserved1[0x38];
    u32 rxdata[8];
};

struct i2c_spec_values {
    unsigned int min_low_ns;
    unsigned int min_high_ns;
    unsigned int max_rise_ns;
    unsigned int max_fall_ns;
};

static unsigned int i2c_cfg;

static void _udelay(volatile int64_t x){
    x*=10000;
    while(x--);
}


static inline void rk_i2c_disable(void)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    writel(0, &regs->ien);
    writel(I2C_IPD_ALL_CLEAN, &regs->ipd);
    writel(0, &regs->con);
}

static int rk_i2c_send_start_bit(u32 con)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    ulong start;

    debug("I2c Send Start bit.\n");
    writel(I2C_IPD_ALL_CLEAN, &regs->ipd);

    writel(I2C_STARTIEN, &regs->ien);
    writel(I2C_CON_EN | I2C_CON_START | i2c_cfg | con, &regs->con);

    start = I2C_TIMEOUT_MS;
    while (1) {
        if (readl(&regs->ipd) & I2C_STARTIPD) {
            writel(I2C_STARTIPD, &regs->ipd);
            break;
        }
        if (!start--) {
            debug("I2C Send Start Bit Timeout\n");
            return -1;
        }
        _udelay(1);
    }

    /* clean start bit */
    writel(I2C_CON_EN | i2c_cfg | con, &regs->con);

    return 0;
}

static int rk_i2c_send_stop_bit(void)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    ulong start;

    debug("I2c Send Stop bit.\n");
    writel(I2C_IPD_ALL_CLEAN, &regs->ipd);

    writel(I2C_CON_EN | i2c_cfg | I2C_CON_STOP, &regs->con);
    writel(I2C_CON_STOP, &regs->ien);

    start = I2C_TIMEOUT_MS;
    while (1) {
        if (readl(&regs->ipd) & I2C_STOPIPD) {
            writel(I2C_STOPIPD, &regs->ipd);
            break;
        }
        if (!start--) {
            debug("I2C Send Start Bit Timeout\n");
            return -1;
        }
        _udelay(1);
    }

    _udelay(1);
    return 0;
}


static int rk3506_i2c_write(uint8_t chip, uint reg, uint r_len,
            uchar *buf, uint b_len)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    int err = 0;
    uchar *pbuf = buf;
    uint bytes_remain_len = b_len + r_len + 1;
    uint bytes_xferred = 0;
    uint words_xferred = 0;
    bool next = false;
    ulong start;
    uint txdata;
    uint i, j;

    debug("rk_i2c_write: chip = %d, reg = %d, r_len = %d, b_len = %d\n",
          chip, reg, r_len, b_len);

    while (bytes_remain_len) {
        if (bytes_remain_len > RK_I2C_FIFO_SIZE)
            bytes_xferred = RK_I2C_FIFO_SIZE;
        else
            bytes_xferred = bytes_remain_len;
        words_xferred = DIV_ROUND_UP(bytes_xferred, 4);

        for (i = 0; i < words_xferred; i++) {
            txdata = 0;
            for (j = 0; j < 4; j++) {
                if ((i * 4 + j) == bytes_xferred)
                    break;

                if (i == 0 && j == 0 && pbuf == buf) {
                    txdata |= (chip << 1);
                } else if (i == 0 && j <= r_len && pbuf == buf) {
                    txdata |= (reg &
                        (0xff << ((j - 1) * 8))) << 8;
                } else {
                    txdata |= (*pbuf++)<<(j * 8);
                }
            }
            writel(txdata, &regs->txdata[i]);
            debug("I2c Write TXDATA[%d] = 0x%08x\n", i, txdata);
        }

        /* If the write is the first, need to send start bit */
        if (!next) {
            err = rk_i2c_send_start_bit(I2C_CON_EN |
                       I2C_CON_MOD(I2C_MODE_TX));
            if (err)
                return err;
            next = true;
        } else {
            writel(I2C_CON_EN | I2C_CON_MOD(I2C_MODE_TX) | i2c_cfg,
                   &regs->con);
        }
        writel(I2C_MBTFIEN | I2C_NAKRCVIEN, &regs->ien);
        writel(bytes_xferred, &regs->mtxcnt);
		
        start = I2C_TIMEOUT_MS;
        while (1) {
            if (readl(&regs->ipd) & I2C_NAKRCVIPD) {
                writel(I2C_NAKRCVIPD, &regs->ipd);
                err = -1;
                goto i2c_exit;
            }
            if (readl(&regs->ipd) & I2C_MBTFIPD) {
                writel(I2C_MBTFIPD, &regs->ipd);
                break;
            }
            if (!start--) {
                debug("I2C Write Data Timeout\n");
                err =  -1;
                goto i2c_exit;
            }
            _udelay(1);
        }

        bytes_remain_len -= bytes_xferred;
        debug("I2C Write bytes_remain_len %d\n", bytes_remain_len);
    }

i2c_exit:
    return err;
}

static int rk3506_i2c_read(uchar chip, uint reg, uint r_len,
               uchar *buf, uint b_len, bool snd)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    uchar *pbuf = buf;
    uint bytes_remain_len = b_len;
    uint bytes_xferred = 0;
    uint words_xferred = 0;
    ulong start;
    uint con = 0;
    uint rxdata;
    uint i, j;
    int err = 0;
    bool snd_chunk = false;

    debug("rk_i2c_read: chip = %d, reg = %d, r_len = %d, b_len = %d\n",
          chip, reg, r_len, b_len);

    /* If the second message for TRX read, resetting internal state. */
    if (snd)
        writel(0, &regs->con);

    writel(I2C_MRXADDR_SET(1, chip << 1 | 1), &regs->mrxaddr);
    if (r_len == 0) {
        writel(0, &regs->mrxraddr);
    } else if (r_len < 4) {
        writel(I2C_MRXRADDR_SET(r_len, reg), &regs->mrxraddr);
    } else {
        debug("I2C Read: addr len %d not supported\n", r_len);
        return -1;
    }

    while (bytes_remain_len) {
        if (bytes_remain_len > RK_I2C_FIFO_SIZE) {
            con = I2C_CON_EN;
            bytes_xferred = 32;
        } else {
            /*
             * The hw can read up to 32 bytes at a time. If we need
             * more than one chunk, send an ACK after the last byte.
             */
            con = I2C_CON_EN | I2C_CON_LASTACK;
            bytes_xferred = bytes_remain_len;
        }
        words_xferred = DIV_ROUND_UP(bytes_xferred, 4);

        /*
         * make sure we are in plain RX mode if we read a second chunk;
         * and first rx read need to send start bit.
         */
        if (snd_chunk) {
            con |= I2C_CON_MOD(I2C_MODE_RX);
            writel(con | i2c_cfg, &regs->con);
        } else {
            con |= I2C_CON_MOD(I2C_MODE_TRX);
            err = rk_i2c_send_start_bit(con);
            if (err)
                return err;
        }

        writel(I2C_MBRFIEN | I2C_NAKRCVIEN, &regs->ien);
        writel(bytes_xferred, &regs->mrxcnt);

        start = I2C_TIMEOUT_MS;
        while (1) {
            if (readl(&regs->ipd) & I2C_NAKRCVIPD) {
                writel(I2C_NAKRCVIPD, &regs->ipd);
                err = -1;
                goto i2c_exit;
            }
            if (readl(&regs->ipd) & I2C_MBRFIPD) {
                writel(I2C_MBRFIPD, &regs->ipd);
                break;
            }
            if (!start--) {
                debug("I2C Read Data Timeout\n");
                err =  -1;
                goto i2c_exit;
            }
            _udelay(1);
        }

        for (i = 0; i < words_xferred; i++) {
            rxdata = readl(&regs->rxdata[i]);
            debug("I2c Read RXDATA[%d] = 0x%x\n", i, rxdata);
            for (j = 0; j < 4; j++) {
                if ((i * 4 + j) == bytes_xferred)
                    break;
                *pbuf++ = (rxdata >> (j * 8)) & 0xff;
            }
        }

        bytes_remain_len -= bytes_xferred;
        snd_chunk = true;
        debug("I2C Read bytes_remain_len %d\n", bytes_remain_len);
    }

i2c_exit:
    return err;
}

static unsigned int rk3x_i2c_get_version()
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    uint version;

    version = readl(&regs->con) & I2C_CON_VERSION;

    return version >>= I2C_CON_VERSION_SHIFT;
}

static const struct i2c_spec_values standard_mode_spec = {
    .min_low_ns = 4700,
    .min_high_ns = 4000,
    .max_rise_ns = 1000,
    .max_fall_ns = 300,
};

static const struct i2c_spec_values fast_mode_spec = {
    .min_low_ns = 1300,
    .min_high_ns = 600,
    .max_rise_ns = 300,
    .max_fall_ns = 300,
};

static const struct i2c_spec_values fast_modeplus_spec = {
    .min_low_ns = 500,
    .min_high_ns = 260,
    .max_rise_ns = 120,
    .max_fall_ns = 120,
};

static const struct i2c_spec_values *rk_i2c_get_spec(unsigned int speed)
{
    if (speed == 1000)
        return &fast_modeplus_spec;
    else if (speed == 400)
        return &fast_mode_spec;
    else
        return &standard_mode_spec;
}


static int rk_i2c_adapter_clk(unsigned int scl_rate)
{
    struct i2c_regs *regs = (struct i2c_regs*)I2C_BASE;
    const struct i2c_spec_values *spec;
    unsigned int min_total_div, min_low_div, min_high_div, min_hold_div;
    unsigned int low_div, high_div, extra_div, extra_low_div;
    unsigned int min_low_ns, min_high_ns;
    unsigned int start_setup = 0;
    unsigned int i2c_rate = 10000000;
    unsigned int speed;

    debug("rk_i2c_set_clk: i2c rate = %d, scl rate = %d\n", i2c_rate,
          scl_rate);

    if (scl_rate <= 100000 && scl_rate >= 1000) {
        start_setup = 1;
        speed = 100;
    } else if (scl_rate <= 400000 && scl_rate >= 100000) {
        speed = 400;
    } else if (scl_rate <= 1000000 && scl_rate > 400000) {
        speed = 1000;
    } else {
        debug("invalid i2c speed : %d\n", scl_rate);
        return -1;
    }

    spec = rk_i2c_get_spec(speed);
    i2c_rate = DIV_ROUND_UP(i2c_rate, 1000);
    speed = DIV_ROUND_UP(scl_rate, 1000);

    min_total_div = DIV_ROUND_UP(i2c_rate, speed * 8);

    min_high_ns = spec->max_rise_ns + spec->min_high_ns;
    min_high_div = DIV_ROUND_UP(i2c_rate * min_high_ns, 8 * 1000000);

    min_low_ns = spec->max_fall_ns + spec->min_low_ns;
    min_low_div = DIV_ROUND_UP(i2c_rate * min_low_ns, 8 * 1000000);

    min_high_div = (min_high_div < 1) ? 2 : min_high_div;
    min_low_div = (min_low_div < 1) ? 2 : min_low_div;

    min_hold_div = min_high_div + min_low_div;

    if (min_hold_div >= min_total_div) {
        high_div = min_high_div;
        low_div = min_low_div;
    } else {
        extra_div = min_total_div - min_hold_div;
        extra_low_div = DIV_ROUND_UP(min_low_div * extra_div,
                         min_hold_div);

        low_div = min_low_div + extra_low_div;
        high_div = min_high_div + (extra_div - extra_low_div);
    }

    high_div--;
    low_div--;

    if (high_div > 0xffff || low_div > 0xffff)
        return -1;
    /* 1 for data hold/setup time is enough */
    i2c_cfg = I2C_CON_SDA_CFG(1) | I2C_CON_STA_CFG(start_setup);
    writel((high_div << I2C_CLK_DIV_HIGH_SHIFT) | low_div,
           &regs->clkdiv);

    debug("set clk(I2C_TIMING: 0x%08x %d %d)\n", i2c_cfg, high_div, low_div);
    debug("set clk(I2C_CLKDIV: 0x%08x)\n", readl(&regs->clkdiv));

    return 0;
}

int rk_i2c_read(uint16_t addr, uint32_t reg, uint8_t* buf, int size, int flag){
	int ret;
	i2c_cfg |= I2C_CON_ACTACK;
	ret = rk3506_i2c_read(addr, reg, 1, buf, size, false);
	rk_i2c_send_stop_bit();
	rk_i2c_disable();
	return ret;
}

int rk_i2c_write(uint16_t addr, uint32_t reg, uint8_t* buf, int size, int flag){
	int ret;
	i2c_cfg |= I2C_CON_ACTACK;
	ret = rk3506_i2c_write(addr, reg, 1, buf, size);
	rk_i2c_send_stop_bit();
	rk_i2c_disable();
	return ret;
}

void rk_i2c_init(int bus){
	_mmio_base = mmio_map();
	rk_gpio_init();

	rk_gpio_config(10, 30);
	rk_gpio_config(11, 31);

	i2c_cfg = 0;
	I2C_BASE = _mmio_base + 0x040000;
	rk_i2c_adapter_clk(10000);
}

