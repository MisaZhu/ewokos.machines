#include <types.h>
#include <utils/log.h>

#include "mmc.h"
#include "sdio.h"
#include "sdhci.h"


#define MMC_DEBUG	0
#define mmc_host_is_spi(mmc)	(0)

static struct mmc _mmc;

/*
 * DDR50 is deliberately NOT attempted here even though both sdio2 and the
 * CYW43455 advertise it: UHS DDR50 requires a 1.8V signalling switch plus
 * data-line tuning, and this WiFi bus is a fixed 3.3V rail with neither.
 * The CMD52-based CCCR readback used to verify the clock ladder below
 * passes even when the DDR data path is broken (the CMD line stays SDR),
 * so DDR50 would be "accepted" and every CMD53 data transfer would then
 * time out during chip attach. HS SDR 50MHz is the proven operating point
 * for this chip family.
 */

int mmc_io_rw_direct_host(int write, unsigned fn,
	unsigned addr, uint8_t in, uint8_t *out)
{
	struct mmc_cmd cmd = {};
	int err;

	/* sanity check */
	if (addr & ~0x1FFFF)
		return -EINVAL;

	cmd.cmdidx = SD_IO_RW_DIRECT;
	cmd.cmdarg = write ? 0x80000000 : 0x00000000;
	cmd.cmdarg |= fn << 28;
	cmd.cmdarg |= (write && out) ? 0x08000000 : 0x00000000;
	cmd.cmdarg |= addr << 9;
	cmd.cmdarg |= in;
	cmd.resp_type = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_AC;

	err = sdhci_send_command(&cmd, 0);

#if MMC_DEBUG
	if(out)
        brcm_log("%s w:%d f:%d a:%x in:%x out:%x\n", __func__, write, fn, addr, in, cmd.response[0] & 0xFF);
    else
        brcm_log("%s w:%d f:%d a:%x in:%x\n", __func__, write, fn, addr, in);
#endif

	if (err){
		brcm_log("cmd52 transport fail w=%d fn=%u addr=0x%x in=0x%x err=%d stat_resp=0x%x\n",
			write, fn, addr, in, err, cmd.response[0]);
                // #region debug-point A/D:mmc-cmd52-fail
                brcm_log("debug cmd52 fail fn=%u addr=0x%x write=%d clock=%u width=%u mode=%u cmdarg=0x%08x\n",
                        fn, addr, write, _mmc.clock, _mmc.bus_width,
                        _mmc.selected_mode, cmd.cmdarg);
                // #endregion
		return err;
	}

	if (cmd.response[0] & R5_ERROR){
		brcm_log("cmd52 r5 error w=%d fn=%u addr=0x%x resp=0x%x\n",
			write, fn, addr, cmd.response[0]);
		return -EIO;
	}if (cmd.response[0] & R5_FUNCTION_NUMBER){
		brcm_log("cmd52 fn error w=%d fn=%u addr=0x%x resp=0x%x\n",
			write, fn, addr, cmd.response[0]);
		return -EINVAL;
	}if (cmd.response[0] & R5_OUT_OF_RANGE){
		brcm_log("cmd52 range error w=%d fn=%u addr=0x%x resp=0x%x\n",
			write, fn, addr, cmd.response[0]);
		return -ERANGE;
	}

	if (out) {
		if (mmc_host_is_spi(host))
			*out = (cmd.response[0] >> 8) & 0xFF;
		else
			*out = cmd.response[0] & 0xFF;
	}

	return 0;
}

int mmc_io_rw_direct( int write, unsigned fn,
	unsigned addr, uint8_t in, uint8_t *out)
{
	return mmc_io_rw_direct_host(write, fn, addr, in, out);
}

int mmc_io_rw_extended(int write, int fn,
	unsigned addr, int incr_addr, uint8_t *buf, unsigned blocks, unsigned blksz)
{
	struct mmc_cmd cmd = {};
	struct mmc_data data = {};
	int err;

	WARN_ON(blksz == 0);

	/* sanity check */
	if (addr & ~0x1FFFF)
		return -EINVAL;

	cmd.cmdidx = SD_IO_RW_EXTENDED;
	cmd.cmdarg = write ? 0x80000000 : 0x00000000;
	cmd.cmdarg |= fn << 28;
	cmd.cmdarg |= incr_addr ? 0x04000000 : 0x00000000;
	cmd.cmdarg |= addr << 9;
	if (blocks == 0)
		cmd.cmdarg |= (blksz == 512) ? 0 : blksz;	/* byte mode */
	else
		cmd.cmdarg |= 0x08000000 | blocks;		/* block mode */
	cmd.resp_type = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

	data.blocksize = blksz;
	/* Code in host drivers/fwk assumes that "blocks" always is >=1 */
	data.blocks = blocks ? blocks : 1;
	data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
    data.src = buf;

    err = sdhci_send_command(&cmd, &data);

#if MMC_DEBUG
	if(fn == 2){ //dont dump interrupt and console data
		brcm_log("%s w:%d f:%d a:%x%s b:%d s:%d r:%d ",
			__func__, write, fn, addr, incr_addr?"+":" ", blocks, blksz, err);
		if(blksz <= 4 || fn != 2)
			brcm_log("[%02x %02x %02x %02x]\n", buf[0], buf[1], buf[2], buf[3]);
		else
			hexdump("", buf, min(blksz, 256));
	}
#endif
	if (err){
		return err;
	}

    if (cmd.response[0] & R5_ERROR)
		err = -EIO;
	else if (cmd.response[0] & R5_FUNCTION_NUMBER)
		err = -EINVAL;
	else if (cmd.response[0] & R5_OUT_OF_RANGE)
		err = -ERANGE;
	else
		err = 0;

	return err;
}

static int mmc_go_idle(void)
{
	struct mmc_cmd cmd;
	int err;

	usleep(1000);

	cmd.cmdidx = MMC_CMD_GO_IDLE_STATE;
	cmd.cmdarg = 0;
	cmd.resp_type = MMC_RSP_NONE;

	err = sdhci_send_command(&cmd, NULL);

	if (err)
		return err;

	return 0;
}

/*
* dump form linux kernel
*/
static int mmc_sdio_set_bus_width(uint8_t width)
{
	uint8_t if_ctrl;
	int err;

	err = mmc_io_rw_direct(0, 0, SDIO_CCCR_IF, 0, &if_ctrl);
	if (err)
		return err;

	if_ctrl &= ~SDIO_BUS_WIDTH_MASK;
	if_ctrl |= (width == 4) ? SDIO_BUS_WIDTH_4BIT : SDIO_BUS_WIDTH_1BIT;

	err = mmc_io_rw_direct(1, 0, SDIO_CCCR_IF, if_ctrl, NULL);
	if (err)
		return err;

	_mmc.bus_width = width;
	return sdhci_set_ios(&_mmc);
}

int mmc_configure_sdio_bus(uint8_t width, uint32_t clock)
{
	int err;

	err = mmc_sdio_set_bus_width(width);
	if (err)
		return err;

	_mmc.clock = clock;
	_mmc.selected_mode = MMC_LEGACY;
	return sdhci_set_ios(&_mmc);
}

static int brcm_init(void)
{
	struct mmc_cmd cmd = {};
	int err;

	usleep(1000); 

	cmd.cmdidx = 5;
	cmd.cmdarg = 0x0;
	cmd.resp_type = MMC_RSP_SPI_R4 | MMC_RSP_R4 | MMC_CMD_BCR;

	err = sdhci_send_command(&cmd, NULL);

	if (err)
		return err;

	cmd.cmdidx = 5;
	cmd.cmdarg = 0x300000;
	cmd.resp_type = MMC_RSP_SPI_R4 | MMC_RSP_R4 | MMC_CMD_BCR;

	err = sdhci_send_command(&cmd, NULL);

	if (err)
		return err;

	cmd.cmdidx = 3;
	cmd.cmdarg = 0x0;
	cmd.resp_type = MMC_RSP_R6 | MMC_CMD_BCR;

	err = sdhci_send_command(&cmd, NULL);

	if (err)
		return err;

	_mmc.rca = (cmd.response[0] >> 16) & 0xffff;
	if (_mmc.rca == 0)
		_mmc.rca = 1;

	cmd.cmdidx = 7;
	cmd.cmdarg = ((uint32_t)_mmc.rca) << 16;
	cmd.resp_type = MMC_RSP_R1 | MMC_CMD_AC;

	err = sdhci_send_command(&cmd, NULL);

	if (err)
		return err;

        /*
         * Keep the Pi5 WLAN path in legacy timing for now. Runtime evidence
         * shows fn0 CCCR traffic and func1 enable succeed, but the very first
         * func1 CHIPCLKCSR CMD52 immediately times out once the bus has moved
         * into the HS/SDR25 path. Hold the host/card at the proven 25MHz
         * legacy timing point until func1/backplane traffic is stable.
         */
	err = mmc_sdio_set_bus_width(4);
	if (err)
		return err;

        /*
         * Verify the Pi5 SDIO2 bus at a conservative rate before handing it
         * to the probe path. A single fn0 CCCR read can pass at 50MHz while
         * later fn1/backplane accesses still time out during chip attach, so
         * prefer the proven 25MHz point here instead of optimistically
         * starting at 50MHz and failing deep in the probe.
         */
        {
                static const uint32_t try_clks[] =
                        { 25000000, 10000000 };
                unsigned int i;
                uint8_t cccr_rev;

                err = -EIO;
                for (i = 0; i < sizeof(try_clks)/sizeof(try_clks[0]); i++) {
                        _mmc.clock = try_clks[i];
                        _mmc.selected_mode = MMC_LEGACY;
                        err = sdhci_set_ios(&_mmc);
                        if (err)
                                continue;

                        err = mmc_io_rw_direct(0, 0, SDIO_CCCR_CCCR, 0, &cccr_rev);
                        if (!err)
                                break;

                        brcm_log("sdio bus dead at %uHz, stepping down\n",
                                 try_clks[i]);
                }
                if (err) {
                        brcm_log("sdio bus unusable at any clock, err=%d\n", err);
                        return err;
                }
                brcm_log("sdio bus running at %uHz, mode %s\n", _mmc.clock,
                         "legacy");
        }

	return 0;
}


static int mmc_init_card(void)
{
	int err;

	err = mmc_go_idle();
	if (err)
		return err;
	usleep(200000);

	err = brcm_init();
	if (err)
		return err;
	return 0;
}

int mmc_hw_reset(void)
{
    sdhci_init();
    _mmc.bus_width = 1;
    _mmc.clock = 400000;
    _mmc.ocr = 1;
	_mmc.voltages =  MMC_VDD_32_33 | MMC_VDD_33_34|MMC_VDD_165_195;
	_mmc.selected_mode = MMC_LEGACY;

	if (sdhci_set_ios(&_mmc))
		return -EIO;

	return mmc_init_card();
}
