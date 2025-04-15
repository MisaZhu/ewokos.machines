#include <stdbool.h>
#include <ewoksys/mmio.h>

#define DEBUG_RK_SPI 0
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

//#define debug printf
#define debug(...) {}
#define writel(v,a)     (*(volatile uint32_t *)(a) = (v))
#define readl(a)    (*(volatile uint32_t *)(a))
#define writew(v,a)     (*(volatile uint16_t *)(a) = (v))
#define readw(a)    (*(volatile uint16_t *)(a))
#define writeb(v,a)     (*(volatile uint8_t *)(a) = (v))
#define readb(a)    (*(volatile uint8_t *)(a))

#define BIT(x)	(0x1<<(x))

/* SPI mode flags */
#define SPI_CPHA    BIT(0)          /* clock phase */
#define SPI_CPOL    BIT(1)          /* clock polarity */
#define SPI_MODE_0  (0|0)           /* (original MicroWire) */
#define SPI_MODE_1  (0|SPI_CPHA)
#define SPI_MODE_2  (SPI_CPOL|0)
#define SPI_MODE_3  (SPI_CPOL|SPI_CPHA)
#define SPI_CS_HIGH BIT(2)          /* CS active high */
#define SPI_LSB_FIRST   BIT(3)          /* per-word bits-on-wire */
#define SPI_3WIRE   BIT(4)          /* SI/SO signals shared */
#define SPI_LOOP    BIT(5)          /* loopback mode */
#define SPI_SLAVE   BIT(6)          /* slave mode */
#define SPI_PREAMBLE    BIT(7)          /* Skip preamble bytes */
#define SPI_TX_BYTE BIT(8)          /* transmit with 1 wire byte */
#define SPI_TX_DUAL BIT(9)          /* transmit with 2 wires */
#define SPI_TX_QUAD BIT(10)         /* transmit with 4 wires */
#define SPI_RX_SLOW BIT(11)         /* receive with 1 wire slow */
#define SPI_RX_DUAL BIT(12)         /* receive with 2 wires */
#define SPI_RX_QUAD BIT(13)         /* receive with 4 wires */
#define SPI_TX_OCTAL    BIT(14)         /* transmit with 8 wires */
#define SPI_RX_OCTAL    BIT(15)         /* receive with 8 wires */
#define SPI_DMA_PREPARE BIT(24)         /* dma transfer skip waiting idle, read without cache invalid */

#define SPI_XFER_BEGIN      BIT(0)  /* Assert CS before transfer */
#define SPI_XFER_END        BIT(1)  /* Deassert CS after transfer */
#define SPI_XFER_ONCE       (SPI_XFER_BEGIN | SPI_XFER_END)
#define SPI_XFER_MMAP       BIT(2)  /* Memory Mapped start */
#define SPI_XFER_MMAP_END   BIT(3)  /* Memory Mapped End */
#define SPI_XFER_PREPARE    BIT(7)  /* Transfer skip waiting idle */

typedef  volatile uint32_t u32;
typedef  uint16_t u16;
typedef  uint8_t u8;
typedef  unsigned long ulong;
typedef  unsigned int uint;

struct rockchip_spi {
    u32 ctrlr0;
    u32 ctrlr1;
    u32 enr;
    u32 ser;
    u32 baudr;
    u32 txftlr;
    u32 rxftlr;
    u32 txflr;
    u32 rxflr;
    u32 sr;
    u32 ipr;
    u32 imr;
    u32 isr;
    u32 risr;
    u32 icr;
    u32 dmacr;
    u32 dmatdlr;
    u32 dmardlr;        /* 0x44 */
    u32 reserved[0xef];
    u32 txdr[0x100];    /* 0x400 */
    u32 rxdr[0x100];    /* 0x800 */
};

/* CTRLR0 */
enum {
    DFS_SHIFT   = 0,    /* Data Frame Size */
    DFS_MASK    = 3,
    DFS_4BIT    = 0,
    DFS_8BIT,
    DFS_16BIT,
    DFS_RESV,

    CFS_SHIFT   = 2,    /* Control Frame Size */
    CFS_MASK    = 0xf,

    SCPH_SHIFT  = 6,    /* Serial Clock Phase */
    SCPH_MASK   = 1,
    SCPH_TOGMID = 0,    /* SCLK toggles in middle of first data bit */
    SCPH_TOGSTA,        /* SCLK toggles at start of first data bit */

    SCOL_SHIFT  = 7,    /* Serial Clock Polarity */
    SCOL_MASK   = 1,
    SCOL_LOW    = 0,    /* Inactive state of serial clock is low */
    SCOL_HIGH,      /* Inactive state of serial clock is high */

    CSM_SHIFT   = 8,    /* Chip Select Mode */
    CSM_MASK    = 0x3,
    CSM_KEEP    = 0,    /* ss_n stays low after each frame  */
    CSM_HALF,       /* ss_n high for half sclk_out cycles */
    CSM_ONE,        /* ss_n high for one sclk_out cycle */
    CSM_RESV,

    SSN_DELAY_SHIFT = 10,   /* SSN to Sclk_out delay */
    SSN_DELAY_MASK  = 1,
    SSN_DELAY_HALF  = 0,    /* 1/2 sclk_out cycle */
    SSN_DELAY_ONE   = 1,    /* 1 sclk_out cycle */

    SEM_SHIFT   = 11,   /* Serial Endian Mode */
    SEM_MASK    = 1,
    SEM_LITTLE  = 0,    /* little endian */
    SEM_BIG,        /* big endian */

    FBM_SHIFT   = 12,   /* First Bit Mode */
    FBM_MASK    = 1,
    FBM_MSB     = 0,    /* first bit is MSB */
    FBM_LSB,        /* first bit in LSB */

	HALF_WORD_TX_SHIFT = 13,    /* Byte and Halfword Transform */
    HALF_WORD_MASK  = 1,
    HALF_WORD_ON    = 0,    /* apb 16bit write/read, spi 8bit write/read */
    HALF_WORD_OFF,      /* apb 8bit write/read, spi 8bit write/read */

    RXDSD_SHIFT = 14,   /* Rxd Sample Delay, in cycles */
    RXDSD_MASK  = 3,

    FRF_SHIFT   = 16,   /* Frame Format */
    FRF_MASK    = 3,
    FRF_SPI     = 0,    /* Motorola SPI */
    FRF_SSP,            /* Texas Instruments SSP*/
    FRF_MICROWIRE,      /* National Semiconductors Microwire */
    FRF_RESV,

    TMOD_SHIFT  = 18,   /* Transfer Mode */
    TMOD_MASK   = 3,
    TMOD_TR     = 0,    /* xmit & recv */
    TMOD_TO,        /* xmit only */
    TMOD_RO,        /* recv only */
    TMOD_RESV,

    OMOD_SHIFT  = 20,   /* Operation Mode */
    OMOD_MASK   = 1,
    OMOD_MASTER = 0,    /* Master Mode */
    OMOD_SLAVE,     /* Slave Mode */
};

/* SR */
enum {
    SR_MASK     = 0x7f,
    SR_BUSY     = 1 << 0,
    SR_TF_FULL  = 1 << 1,
    SR_TF_EMPT  = 1 << 2,
    SR_RF_EMPT  = 1 << 3,
    SR_RF_FULL  = 1 << 4,
};

#define ROCKCHIP_SPI_TIMEOUT_MS     1000

struct rockchip_spi_priv {
    struct rockchip_spi *regs;
    unsigned int max_freq;
    unsigned int mode;
    ulong last_transaction_us;  /* Time of last transaction end */
    u8 bits_per_word;       /* max 16 bits per word */
    u8 n_bytes;
    unsigned int speed_hz;
    unsigned int last_speed_hz;
    uint input_rate;
    uint cr0;
    u32 rsd;            /* Rx sample delay cycles */

    /* quirks */
    u32 max_baud_div_in_cpha;
};

static struct rockchip_spi_priv _spi;

static void rkspi_dump_regs(struct rockchip_spi *regs)
{
    debug("ctrl0: \t\t0x%08x\n", readl(&regs->ctrlr0));
    debug("ctrl1: \t\t0x%08x\n", readl(&regs->ctrlr1));
    debug("ssienr: \t\t0x%08x\n", readl(&regs->enr));
    debug("ser: \t\t0x%08x\n", readl(&regs->ser));
    debug("baudr: \t\t0x%08x\n", readl(&regs->baudr));
    debug("txftlr: \t\t0x%08x\n", readl(&regs->txftlr));
    debug("rxftlr: \t\t0x%08x\n", readl(&regs->rxftlr));
    debug("txflr: \t\t0x%08x\n", readl(&regs->txflr));
    debug("rxflr: \t\t0x%08x\n", readl(&regs->rxflr));
    debug("sr: \t\t0x%08x\n", readl(&regs->sr));
    debug("imr: \t\t0x%08x\n", readl(&regs->imr));
    debug("isr: \t\t0x%08x\n", readl(&regs->isr));
    debug("dmacr: \t\t0x%08x\n", readl(&regs->dmacr));
    debug("dmatdlr: \t0x%08x\n", readl(&regs->dmatdlr));
    debug("dmardlr: \t0x%08x\n", readl(&regs->dmardlr));
}

static void rkspi_set_clk(struct rockchip_spi_priv *priv, uint speed)
{
    /*
     * We should try not to exceed the speed requested by the caller:
     * when selecting a divider, we need to make sure we round up.
     */
    uint clk_div = DIV_ROUND_UP(priv->input_rate, speed);

    /* The baudrate register (BAUDR) is defined as a 32bit register where
     * the upper 16bit are reserved and having 'Fsclk_out' in the lower
     * 16bits with 'Fsclk_out' defined as follows:
     *
     *   Fsclk_out = Fspi_clk/ SCKDV
     *   Where SCKDV is any even value between 2 and 65534.
     */
    if (clk_div > 0xfffe) {
        clk_div = 0xfffe;
    }

    /* Round up to the next even 16bit number */
    clk_div = (clk_div + 1) & 0xfffe;

    debug("spi speed %u, div %u\n", speed, clk_div);

    /* the maxmum divisor is 4 for mode1/3 spi master case for quirks */
   // if (priv->max_baud_div_in_cpha && clk_div > priv->max_baud_div_in_cpha && priv->mode & SPI_CPHA) {
   //     clk_div = priv->max_baud_div_in_cpha;
   //     clk_set_rate(&priv->clk, 4 * speed);
   //     speed = clk_get_rate(&priv->clk);
   // }
	priv->regs->baudr = priv->regs->baudr &  (~0xffff) | clk_div;
    priv->last_speed_hz = speed;
}

static inline int rkspi_wait_till_not_busy(struct rockchip_spi *regs)
{

	int timeout = ROCKCHIP_SPI_TIMEOUT_MS * 1000;
    while (readl(&regs->sr) & SR_BUSY) {
        if (!timeout--) {
            debug("RK SPI: Status keeps busy for 1000us after a read/write!\n");
            return -1;
        }
    }

    return 0;
}

static void rkspi_enable_chip(struct rockchip_spi *regs, bool enable)
{
    writel(enable ? 1 : 0, &regs->enr);
}

static void spi_cs_activate(uint cs)
{
    struct rockchip_spi_priv *priv = &_spi;
    struct rockchip_spi *regs = priv->regs;

    //debug("activate cs%u\n", cs);
    writel(1 << cs, &regs->ser);
}

static void spi_cs_deactivate(uint cs)
{
    struct rockchip_spi_priv *priv = &_spi;
    struct rockchip_spi *regs = priv->regs;

    //debug("deactivate cs%u\n", cs);
    writel(0, &regs->ser);
}

static int rockchip_spi_config(struct rockchip_spi_priv *priv, const void *dout)
{
    struct rockchip_spi *regs = priv->regs;
    uint ctrlr0 = priv->cr0;
    u32 tmod;

    if (dout)
        tmod = TMOD_TR;
    else
        tmod = TMOD_RO;

    ctrlr0 |= (tmod & TMOD_MASK) << TMOD_SHIFT;
    writel(ctrlr0, &regs->ctrlr0);

    return 0;
}


static int rockchip_spi_xfer(int cs, unsigned int bitlen,
               const void *dout, void *din, unsigned long flags)
{
    struct rockchip_spi_priv *priv = &_spi;
    struct rockchip_spi *regs = priv->regs;
    int len = bitlen >> 3;
    const u8 *out = dout;
    u8 *in = din;
    int toread, towrite;
    int ret;

    rockchip_spi_config(priv, dout);

    debug("%s: dout=%08x, din=%08x, len=%d, flags=%lx\n", __func__, dout, din,
          len, flags);
    if (DEBUG_RK_SPI)
        rkspi_dump_regs(regs);

    /* Assert CS before transfer */
    if (flags & SPI_XFER_BEGIN)
        spi_cs_activate(cs);

    while (len > 0) {
        int todo = len;

        rkspi_enable_chip(regs, false);
        writel(todo - 1, &regs->ctrlr1);
        rkspi_enable_chip(regs, true);

        toread = todo;
        towrite = todo;
        while (toread || towrite) {
            u32 status = readl(&regs->sr);

            if (towrite && !(status & SR_TF_FULL)) {
                if (out)
                    writel(out ? *out++ : 0, &regs->txdr);
                towrite--;
            }
            if (toread && !(status & SR_RF_EMPT)) {
                u32 byte = readl(&regs->rxdr);

                if (in)
                    *in++ = byte;
                toread--;
            }
        }
        ret = rkspi_wait_till_not_busy(regs);
        if (ret)
            break;
        len -= todo;
    }

    /* Deassert CS after transfer */
    if (flags & SPI_XFER_END)
        spi_cs_deactivate(cs);

    rkspi_enable_chip(regs, false);
    return ret;
}

static int rockchip_spi_claim_bus(int cs)
{
    struct rockchip_spi_priv *priv = &_spi;
    struct rockchip_spi *regs = priv->regs;
    u8 spi_dfs, spi_tf;
    uint ctrlr0;

    /* Disable the SPI hardware */
    rkspi_enable_chip(regs, 0);

    switch (priv->bits_per_word) {
    case 8:
        priv->n_bytes = 1;
        spi_dfs = DFS_8BIT;
        spi_tf = HALF_WORD_OFF;
        break;
    case 16:
        priv->n_bytes = 2;
        spi_dfs = DFS_16BIT;
        spi_tf = HALF_WORD_ON;
        break;
    default:
        debug("%s: unsupported bits: %dbits\n", __func__,
              priv->bits_per_word);
        return -1;
    }

    if (priv->speed_hz != priv->last_speed_hz)
        rkspi_set_clk(priv, priv->speed_hz);

    /* Operation Mode */
    ctrlr0 = OMOD_MASTER << OMOD_SHIFT;

    /* Data Frame Size */
    ctrlr0 |= spi_dfs << DFS_SHIFT;

    /* set SPI mode 0..3 */
    if (priv->mode & SPI_CPOL)
        ctrlr0 |= SCOL_HIGH << SCOL_SHIFT;
    if (priv->mode & SPI_CPHA)
        ctrlr0 |= SCPH_TOGSTA << SCPH_SHIFT;

    /* Chip Select Mode */
    ctrlr0 |= CSM_KEEP << CSM_SHIFT;

    /* SSN to Sclk_out delay */
    ctrlr0 |= SSN_DELAY_ONE << SSN_DELAY_SHIFT;

    /* Serial Endian Mode */
    ctrlr0 |= SEM_LITTLE << SEM_SHIFT;

    /* First Bit Mode */
    ctrlr0 |= FBM_MSB << FBM_SHIFT;

    /* Byte and Halfword Transform */
    ctrlr0 |= spi_tf << HALF_WORD_TX_SHIFT;

    /* Rxd Sample Delay */
    ctrlr0 |= priv->rsd << RXDSD_SHIFT;

    /* Frame Format */
    ctrlr0 |= FRF_SPI << FRF_SHIFT;

    /* Save static configuration */
    priv->cr0 = ctrlr0;

    writel(ctrlr0, &regs->ctrlr0);

    return 0;
}

int rk_spi_read_write(uint8_t *tx, uint8_t *rx, int len){
	return rockchip_spi_xfer(0 ,len*8, tx, rx, SPI_XFER_BEGIN );
}

int rk_spi_read(uint8_t *buf, int len){
	return rockchip_spi_xfer(0 ,len*8, buf, buf, SPI_XFER_BEGIN );
}

int rk_spi_write(uint8_t *buf, int len){
	return rockchip_spi_xfer(0 ,len*8, buf, 0, SPI_XFER_BEGIN );
}

int rk_spi_init(void){
    _mmio_base = mmio_map();
	rk_gpio_init();
	rk_gpio_config(7, 82);
	rk_gpio_config(6, 83);
	rk_gpio_config(5, 84);
	rk_gpio_config(4, 85);
	rk_gpio_config(3, 1);
	rk_gpio_config(2, 1);

	_spi.regs = (struct rockchip_spi*)(_mmio_base + 0x120000); 
	_spi.input_rate = 187500000;
	_spi.max_freq = 100000000;
	_spi.speed_hz = 100000000;
	_spi.bits_per_word = 8;
	rockchip_spi_claim_bus(0);
	return 0;
}
