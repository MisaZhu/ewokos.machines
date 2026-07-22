#include <stdbool.h>
#include <mm/mmu.h>
#include "spi.h"

#define DEBUG_RK_SPI 0

//#define debug printf
#define debug(...) {}
#define writel(v,a)     (*(volatile uint32_t *)(a) = (v))
#define readl(a)    (*(volatile uint32_t *)(a))
#define writew(v,a)     (*(volatile uint16_t *)(a) = (v))
#define readw(a)    (*(volatile uint16_t *)(a))
#define writeb(v,a)     (*(volatile uint8_t *)(a) = (v))
#define readb(a)    (*(volatile uint8_t *)(a))

static struct rockchip_spi_priv _spi;

static void _delay_us(volatile uint64_t ms){
	while(ms--);
}

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
    uint clk_div = 1;//DIV_ROUND_UP(priv->input_rate, speed);

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

    printf("spi speed %u, div %u\n", speed, clk_div);

    /* the maxmum divisor is 4 for mode1/3 spi master case for quirks */
   // if (priv->max_baud_div_in_cpha && clk_div > priv->max_baud_div_in_cpha && priv->mode & SPI_CPHA) {
   //     clk_div = priv->max_baud_div_in_cpha;
   //     clk_set_rate(&priv->clk, 4 * speed);
   //     speed = clk_get_rate(&priv->clk);
   // }
	priv->regs->baudr = priv->regs->baudr &  (~0xffff) | clk_div;
	printf("%08x\n", priv->regs->baudr);
    priv->last_speed_hz = speed;
}

static int rkspi_wait_till_not_busy(struct rockchip_spi *regs)
{

	int timeout = ROCKCHIP_SPI_TIMEOUT_MS*1000;
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

    //debug("%s: dout=%08x, din=%08x, len=%d, flags=%lx\n", __func__, dout, din,
    //      len, flags);
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

	//static int _cnt  = 0;

	//_cnt++;
	//if(_cnt % 100 == 0)
	//debug(".");

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

int rk_spi_xfer(uint8_t *tx, uint8_t *rx, int len){
	return rockchip_spi_xfer(0 ,len*8, tx, rx, SPI_XFER_BEGIN );
}

int rk_spi_read(uint8_t *buf, int len){
	debug("rx:");
	rockchip_spi_xfer(0 ,len*8, buf, buf, SPI_XFER_BEGIN );
	for(int i = 0; i < len; i++)
	debug("%02x ", buf[i]);
	debug("\n");
	return 0;
}

int rk_spi_write(uint8_t *buf, int len){
	debug("tx:");
	for(int i = 0; i < len; i++)
	debug("%02x ", buf[i]);
	debug("\n");
	rockchip_spi_xfer(0 ,len*8, buf, 0, SPI_XFER_BEGIN );
	return 0;
}

int rk_spi_init(void){
	_spi.regs = (struct rockchip_spi*)(MMIO_BASE + 0x120000); 
	_spi.max_freq = 50000000;
	_spi.speed_hz = 50000000;
	_spi.bits_per_word = 8;
	rockchip_spi_claim_bus(0);
	return 0;
}
