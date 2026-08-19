
#include <stdint.h>

#include <types.h>
#include <string.h>
#include <utils/log.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/mmio.h>

#include "../platform.h"
#include "mmc.h"
#include "sdhci.h"

#define SDHCI_CMD_MAX_TIMEOUT			3200
#define SDHCI_CMD_DEFAULT_TIMEOUT		10000
#define SDHCI_READ_STATUS_TIMEOUT		1000

/*
 * Wall-clock cap for a single data transfer. A 512B*8 CMD53 burst moves
 * in well under 100ms even at init speed; anything longer means the card
 * stalled and the retry/restart machinery should take over instead of
 * an iteration-count loop burning minutes of scheduler yields.
 */
#define SDHCI_DATA_TIMEOUT_MS			2000

#define SDHCI_DMA_ADDRESS	0x00
#define SDHCI_BLOCK_SIZE	0x04
#define  SDHCI_MAKE_BLKSZ(dma, blksz) (((dma & 0x7) << 12) | (blksz & 0xFFF))
#define SDHCI_BLOCK_COUNT	0x06
#define SDHCI_ARGUMENT		0x08
#define SDHCI_TRANSFER_MODE	0x0C
#define  SDHCI_TRNS_DMA		BIT(0)
#define  SDHCI_TRNS_BLK_CNT_EN	BIT(1)
#define  SDHCI_TRNS_ACMD12	BIT(2)
#define  SDHCI_TRNS_READ	BIT(4)
#define  SDHCI_TRNS_MULTI	BIT(5)

#define SDHCI_COMMAND		0x0E
#define  SDHCI_CMD_RESP_MASK	0x03
#define  SDHCI_CMD_CRC		0x08
#define  SDHCI_CMD_INDEX	0x10
#define  SDHCI_CMD_DATA		0x20
#define  SDHCI_CMD_ABORTCMD	0xC0

#define  SDHCI_CMD_RESP_NONE	0x00
#define  SDHCI_CMD_RESP_LONG	0x01
#define  SDHCI_CMD_RESP_SHORT	0x02
#define  SDHCI_CMD_RESP_SHORT_BUSY 0x03

#define SDHCI_MAKE_CMD(c, f) (((c & 0xff) << 8) | (f & 0xff))
#define SDHCI_GET_CMD(c) ((c>>8) & 0x3f)

#define SDHCI_RESPONSE		0x10
#define SDHCI_BUFFER		0x20

#define SDHCI_PRESENT_STATE	0x24
#define  SDHCI_CMD_INHIBIT	BIT(0)
#define  SDHCI_DATA_INHIBIT	BIT(1)
#define  SDHCI_DAT_ACTIVE	BIT(2)
#define  SDHCI_DOING_WRITE	BIT(8)
#define  SDHCI_DOING_READ	BIT(9)
#define  SDHCI_SPACE_AVAILABLE	BIT(10)
#define  SDHCI_DATA_AVAILABLE	BIT(11)
#define  SDHCI_CARD_PRESENT	BIT(16)
#define  SDHCI_CARD_STATE_STABLE	BIT(17)
#define  SDHCI_CARD_DETECT_PIN_LEVEL	BIT(18)
#define  SDHCI_WRITE_PROTECT	BIT(19)
#define  SDHCI_DATA_LVL_MASK	0x00F00000
#define   SDHCI_DATA_0_LVL_MASK BIT(20)

#define SDHCI_HOST_CONTROL	0x28
#define  SDHCI_CTRL_LED		BIT(0)
#define  SDHCI_CTRL_4BITBUS	BIT(1)
#define  SDHCI_CTRL_HISPD	BIT(2)
#define  SDHCI_CTRL_DMA_MASK	0x18
#define   SDHCI_CTRL_SDMA	0x00
#define   SDHCI_CTRL_ADMA1	0x08
#define   SDHCI_CTRL_ADMA32	0x10
#define   SDHCI_CTRL_ADMA64	0x18
#define  SDHCI_CTRL_8BITBUS	BIT(5)
#define  SDHCI_CTRL_CD_TEST_INS	BIT(6)
#define  SDHCI_CTRL_CD_TEST	BIT(7)

#define SDHCI_POWER_CONTROL	0x29
#define  SDHCI_POWER_ON		0x01
#define  SDHCI_POWER_180	0x0A
#define  SDHCI_POWER_300	0x0C
#define  SDHCI_POWER_330	0x0E

#define SDHCI_BLOCK_GAP_CONTROL	0x2A

#define SDHCI_WAKE_UP_CONTROL	0x2B
#define  SDHCI_WAKE_ON_INT	BIT(0)
#define  SDHCI_WAKE_ON_INSERT	BIT(1)
#define  SDHCI_WAKE_ON_REMOVE	BIT(2)

#define SDHCI_CLOCK_CONTROL	0x2C
#define  SDHCI_DIVIDER_SHIFT	8
#define  SDHCI_DIVIDER_HI_SHIFT	6
#define  SDHCI_DIV_MASK	0xFF
#define  SDHCI_DIV_MASK_LEN	8
#define  SDHCI_DIV_HI_MASK	0x300
#define  SDHCI_PROG_CLOCK_MODE  BIT(5)
#define  SDHCI_CLOCK_CARD_EN	BIT(2)
#define  SDHCI_CLOCK_INT_STABLE	BIT(1)
#define  SDHCI_CLOCK_INT_EN	BIT(0)

#define SDHCI_TIMEOUT_CONTROL	0x2E

#define SDHCI_SOFTWARE_RESET	0x2F
#define  SDHCI_RESET_ALL	0x01
#define  SDHCI_RESET_CMD	0x02
#define  SDHCI_RESET_DATA	0x04

#define SDHCI_INT_STATUS	0x30
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_SIGNAL_ENABLE	0x38
#define  SDHCI_INT_RESPONSE	BIT(0)
#define  SDHCI_INT_DATA_END	BIT(1)
#define  SDHCI_INT_DMA_END	BIT(3)
#define  SDHCI_INT_SPACE_AVAIL	BIT(4)
#define  SDHCI_INT_DATA_AVAIL	BIT(5)
#define  SDHCI_INT_CARD_INSERT	BIT(6)
#define  SDHCI_INT_CARD_REMOVE	BIT(7)
#define  SDHCI_INT_CARD_INT	BIT(8)
#define  SDHCI_INT_ERROR	BIT(15)
#define  SDHCI_INT_TIMEOUT	BIT(16)
#define  SDHCI_INT_CRC		BIT(17)
#define  SDHCI_INT_END_BIT	BIT(18)
#define  SDHCI_INT_INDEX	BIT(19)
#define  SDHCI_INT_DATA_TIMEOUT	BIT(20)
#define  SDHCI_INT_DATA_CRC	BIT(21)
#define  SDHCI_INT_DATA_END_BIT	BIT(22)
#define  SDHCI_INT_BUS_POWER	BIT(23)
#define  SDHCI_INT_ACMD12ERR	BIT(24)
#define  SDHCI_INT_ADMA_ERROR	BIT(25)

#define  SDHCI_INT_NORMAL_MASK	0x00007FFF
#define  SDHCI_INT_ERROR_MASK	0xFFFF8000

#define  SDHCI_INT_CMD_MASK	(SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | \
        SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define  SDHCI_INT_DATA_MASK	(SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | \
        SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL | \
        SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | \
        SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)
#define SDHCI_INT_ALL_MASK	((unsigned int)-1)

#define SDHCI_ACMD12_ERR	0x3C
#define SDHCI_AUTO_CMD_STATUS	0x3C

#define SDHCI_HOST_CONTROL2	0x3E
#define  SDHCI_CTRL_UHS_MASK	0x0007
#define  SDHCI_CTRL_UHS_SDR12	0x0000
#define  SDHCI_CTRL_UHS_SDR25	0x0001
#define  SDHCI_CTRL_UHS_SDR50	0x0002
#define  SDHCI_CTRL_UHS_SDR104	0x0003
#define  SDHCI_CTRL_UHS_DDR50	0x0004
#define  SDHCI_CTRL_HS400	0x0005 /* Non-standard */
#define  SDHCI_CTRL_VDD_180	0x0008
#define  SDHCI_CTRL_DRV_TYPE_MASK	0x0030
#define  SDHCI_CTRL_DRV_TYPE_B	0x0000
#define  SDHCI_CTRL_DRV_TYPE_A	0x0010
#define  SDHCI_CTRL_DRV_TYPE_C	0x0020
#define  SDHCI_CTRL_DRV_TYPE_D	0x0030
#define  SDHCI_CTRL_EXEC_TUNING	0x0040
#define  SDHCI_CTRL_TUNED_CLK	0x0080
#define  SDHCI_CTRL_PRESET_VAL_ENABLE	0x8000

#define SDHCI_CAPABILITIES	0x40
#define  SDHCI_TIMEOUT_CLK_MASK	0x0000003F
#define  SDHCI_TIMEOUT_CLK_SHIFT 0
#define  SDHCI_TIMEOUT_CLK_UNIT	0x00000080
#define  SDHCI_CLOCK_BASE_MASK	0x00003F00
#define  SDHCI_CLOCK_V3_BASE_MASK	0x0000FF00
#define  SDHCI_CLOCK_BASE_SHIFT	8
#define  SDHCI_MAX_BLOCK_MASK	0x00030000
#define  SDHCI_MAX_BLOCK_SHIFT  16
#define  SDHCI_CAN_DO_8BIT	BIT(18)
#define  SDHCI_CAN_DO_ADMA2	BIT(19)
#define  SDHCI_CAN_DO_ADMA1	BIT(20)
#define  SDHCI_CAN_DO_HISPD	BIT(21)
#define  SDHCI_CAN_DO_SDMA	BIT(22)
#define  SDHCI_CAN_VDD_330	BIT(24)
#define  SDHCI_CAN_VDD_300	BIT(25)
#define  SDHCI_CAN_VDD_180	BIT(26)
#define  SDHCI_CAN_64BIT	BIT(28)

#define SDHCI_CAPABILITIES_1	0x44
#define  SDHCI_SUPPORT_SDR50	0x00000001
#define  SDHCI_SUPPORT_SDR104	0x00000002
#define  SDHCI_SUPPORT_DDR50	0x00000004
#define  SDHCI_SUPPORT_HS400	BIT(31)
#define  SDHCI_USE_SDR50_TUNING	0x00002000

#define  SDHCI_CLOCK_MUL_MASK	0x00FF0000
#define  SDHCI_CLOCK_MUL_SHIFT	16

#define SDHCI_MAX_CURRENT	0x48

/* 4C-4F reserved for more max current */

#define SDHCI_SET_ACMD12_ERROR	0x50
#define SDHCI_SET_INT_ERROR	0x52

#define SDHCI_ADMA_ERROR	0x54

/* 55-57 reserved */

#define SDHCI_ADMA_ADDRESS	0x58
#define SDHCI_ADMA_ADDRESS_HI	0x5c

/* 60-FB reserved */

#define SDHCI_SLOT_INT_STATUS	0xFC

#define SDHCI_HOST_VERSION	0xFE
#define  SDHCI_VENDOR_VER_MASK	0xFF00
#define  SDHCI_VENDOR_VER_SHIFT	8
#define  SDHCI_SPEC_VER_MASK	0x00FF
#define  SDHCI_SPEC_VER_SHIFT	0
#define   SDHCI_SPEC_100	0
#define   SDHCI_SPEC_200	1
#define   SDHCI_SPEC_300	2


#define SDHCI_MAX_DIV_SPEC_200	256
#define SDHCI_MAX_DIV_SPEC_300	2046
#define SDHCI_QUIRK_32BIT_DMA_ADDR	(1 << 0)
#define SDHCI_QUIRK_REG32_RW		(1 << 1)
#define SDHCI_QUIRK_BROKEN_R1B		(1 << 2)
#define SDHCI_QUIRK_NO_HISPD_BIT	(1 << 3)
#define SDHCI_QUIRK_BROKEN_VOLTAGE	(1 << 4)
#define SDHCI_QUIRK_BROKEN_HISPD_MODE	BIT(5)
#define SDHCI_QUIRK_WAIT_SEND_CMD	(1 << 6)
#define SDHCI_QUIRK_USE_WIDE8		(1 << 8)
#define SDHCI_QUIRK_NO_1_8_V		(1 << 9)
#define SDHCI_QUIRK_SUPPORT_SINGLE	(1 << 10)
#define SDHCI_QUIRK_CAPS_BIT63_FOR_HS400	BIT(11)


#define SDHCI_DEFAULT_BOUNDARY_ARG	(7)

#define SDHCI_GET_VERSION(x) (x->version & SDHCI_SPEC_VER_MASK)

struct sdhci_host {
    const char *name;
    void *ioaddr;
    unsigned int quirks;
    unsigned int host_caps;
    unsigned int version;
    unsigned int max_clk;   /* Maximum Base Clock frequency */
    unsigned int clk_mul;   /* Clock Multiplier value */
    unsigned int clock;
    struct mmc *mmc;
    const struct sdhci_ops *ops;
    int index;

    int bus_width;
    int pwr_gpio;	/* Power GPIO */
    int cd_gpio;		/* Card Detect GPIO */

    unsigned int	voltages;

    struct mmc_config cfg;
    void *align_buffer;
    bool force_align_buffer;
    void* start_addr;
    int flags;
#define USE_SDMA	(0x1 << 0)
#define USE_ADMA	(0x1 << 1)
#define USE_ADMA64	(0x1 << 2)
#define USE_DMA		(USE_SDMA | USE_ADMA | USE_ADMA64)
    unsigned int twoticks_delay;
    unsigned long last_write;
};

static struct sdhci_host _host;

/*
 * Pi5 wlan still uses the userspace SDHCI wrapper, and the data path was
 * left on programmed I/O. Under display/memory pressure that burns too much
 * CPU in the CMD53 hot path and RX backlog eventually reaches the software
 * queue high watermark. Move CMD53 payloads through a dedicated SDMA bounce
 * buffer so the controller streams data while the worker stays available to
 * drain the protocol stack.
 *
 * BCM2712 SDIO2 sees normal ARM physical addresses in its 32-bit system
 * address register, so unlike the legacy bcm2711 eMMC2 path no bus alias is
 * required here.
 */
#define SDHCI_SDMA_BOUNCE_SIZE			(64 * 1024)
#define SDHCI_SDMA_TIMEOUT_MS			1000

static uint8_t *_sdma_bounce;
static uint32_t _sdma_bounce_phys;
static bool _sdma_unavailable;
/*
 * SDMA is forced on for this controller (see sdhci_get_info): BCM2712 SDIO2
 * does not reliably advertise SDHCI_CAN_DO_SDMA, and leaving the data path on
 * PIO copies the FIFO 4 bytes per MMIO read, pinning WLAN throughput in the
 * few-hundred-KB/s range no matter the bus clock. A single transient
 * controller error must not permanently drop back to that slow PIO copy, so
 * only disable SDMA after this many consecutive failures; any success resets
 * the streak.
 */
#define SDHCI_SDMA_FAIL_LIMIT	4
static uint32_t _sdma_fail_streak;
static bool _xfer_path_logged;

static int sdhci_sdma_init(void)
{
    ewokos_addr_t vaddr;
    ewokos_addr_t phys;

    if (_sdma_bounce != NULL)
        return 0;
    if (_sdma_unavailable)
        return -1;

    vaddr = dma_alloc(0, SDHCI_SDMA_BOUNCE_SIZE);
    if (vaddr == 0) {
        _sdma_unavailable = true;
        brcm_log("sdhci: dma_alloc failed for %u-byte SDMA bounce buffer\n",
                (unsigned)SDHCI_SDMA_BOUNCE_SIZE);
        return -1;
    }

    phys = dma_phy_addr(0, vaddr);
    if (phys == 0 || phys > 0xffffffffUL) {
        dma_free(0, vaddr);
        _sdma_unavailable = true;
        brcm_log("sdhci: invalid SDMA phys addr v=%p phys=0x%lx\n",
                (void *)(uintptr_t)vaddr, (unsigned long)phys);
        return -1;
    }

    _sdma_bounce = (uint8_t *)(uintptr_t)vaddr;
    _sdma_bounce_phys = (uint32_t)phys;
    return 0;
}

/*
 * Base clock feeding the sdio2 SDHCI (clk_emmc2, fixed 200 MHz in
 * bcm2712.dtsi). The divider math in sdhci_set_clock() is only correct
 * when this matches the real rate, so main() asks the firmware first and
 * falls back to the dts value; garbage outside the sanity window is
 * ignored here.
 */
static uint32_t _sdhci_base_clk = 0;

void sdhci_set_base_clock(uint32_t hz)
{
    /* Sanity window for the sdio2 core; ignore mailbox garbage. */
    if (hz >= 25000000 && hz <= 500000000)
        _sdhci_base_clk = hz;
}

static void bcm2712_sdhci_gpio_init(void){
    /* GPIO30-35 -> "sd2" (clk/cmd/dat0-3) plus dts pulls; the Pi4
     * GPCLK2 32kHz scheme does not exist on Pi5 (nothing in the dts). */
    pi5_platform_pins();
    usleep(20000);
}

/*
 * Raw register access. The proven bcm2712 SDHCI path in
 * system/libs/arch_bcm2712 still keeps a short post-write delay and
 * documents it as necessary for the BCM2835/BCM2712 host controller.
 * Mirror that behavior here: runtime evidence shows Pi5 WLAN gets through
 * enumeration/function-enable but the very first func1 control CMD52 fails,
 * which matches posted writes not fully settling before the command path is
 * exercised.
 */
static inline void bcm2712_sdhci_raw_writel(struct sdhci_host *host, uint32_t val,
                        int reg)
{
        volatile int delay = 20;
    writel(val, host->ioaddr + reg);
        while (delay--)
                ;
}

static inline uint32_t bcm2712_sdhci_raw_readl(struct sdhci_host *host, int reg)
{
    return readl(host->ioaddr + reg);
}


static void sdhci_writeb(struct sdhci_host *host, uint8_t val, uint32_t reg){
    uint32_t oldval = bcm2712_sdhci_raw_readl(host, reg & ~3);
    uint32_t byte_num = reg & 3;
    uint32_t byte_shift = byte_num * 8;
    uint32_t mask = 0xff << byte_shift;
    uint32_t newval = (oldval & ~mask) | (val << byte_shift);

    bcm2712_sdhci_raw_writel(&_host, newval, reg & ~3);
}

static uint8_t sdhci_readb(struct sdhci_host *host, uint32_t reg){
    uint32_t val = bcm2712_sdhci_raw_readl(host, (reg & ~3));
    uint32_t byte_num = reg & 3;
    uint32_t byte_shift = byte_num * 8;
    uint32_t byte = (val >> byte_shift) & 0xff;

    return byte;
}

static void sdhci_writew(struct sdhci_host *host, uint16_t val, uint32_t reg){
    static uint32_t shadow;
    uint32_t oldval = (reg == SDHCI_COMMAND) ? shadow :
        bcm2712_sdhci_raw_readl(host, reg & ~3);
    uint32_t word_num = (reg >> 1) & 1;
    uint32_t word_shift = word_num * 16;
    uint32_t mask = 0xffff << word_shift;
    uint32_t newval = (oldval & ~mask) | (val << word_shift);

    if (reg == SDHCI_TRANSFER_MODE)
        shadow = newval;
    else
        bcm2712_sdhci_raw_writel(host, newval, reg & ~3);
}

static uint16_t sdhci_readw(struct sdhci_host *host, uint32_t reg){
    uint32_t val = bcm2712_sdhci_raw_readl(host, (reg & ~3));
    uint32_t word_num = (reg >> 1) & 1;
    uint32_t word_shift = word_num * 16;
    uint32_t word = (val >> word_shift) & 0xffff;

    return word;
}

static void sdhci_writel(struct sdhci_host *host, uint32_t val, uint32_t reg){
    bcm2712_sdhci_raw_writel(host, val, reg);
}

static int32_t sdhci_readl(struct sdhci_host *host, uint32_t reg){
    uint32_t val = bcm2712_sdhci_raw_readl(host, reg);
    return val;
}

/*
 * SDHCI_QUIRK_WAIT_SEND_CMD: the card/host needs a minimum wall-clock
 * gap between consecutive commands - the FullMAC firmware must update
 * its SDIO function registers/FIFO state between host transactions.
 * Empirical floor on raspix boards: ~50us breaks the control path
 * (DHCP dies first), 250us class is stable on Zero 2 W / CM4.
 *
 * Implemented as issue-side minimum spacing on the BCM283x 1MHz system
 * timer (SYSTMR_CLO) instead of a fixed post-command busy loop:
 *  - wall-clock: identical gap on every board (busy-loop iteration
 *    time varies ~1.5x between Zero 2 W and CM4 CPUs);
 *  - cheaper: only back-to-back commands pay the gap, the trailing
 *    wait after each command burst disappears (~1 gap saved per frame).
 */
#define SDHCI_MIN_CMD_GAP_US    250
/*
 * The empirical 50us breakage was measured with a single global gap, i.e.
 * with CMD52/F1 register pokes also running that hot; the control-plane
 * accesses are what the firmware is slow to settle after. F2 CMD53 data
 * transfers already burn tens of us of bus time each, so they get a
 * shorter floor while every other command keeps the proven 250us.
 *
 * 25us is the raspix-proven floor for the F2 data path. At the corrected
 * bus clock a whole 1536-byte RX frame moves in ~120us, so a 100us floor
 * per CMD53 was spending nearly half the data-path budget idling on the
 * gap timer alone (2-3 CMD53 per frame).
 */
#define SDHCI_MIN_CMD_GAP_F2_US 25

static uint32_t sdhci_last_cmd_us;
static int sdhci_last_cmd_valid;

/*
 * BCM2712 has no BCM283x 1MHz system timer; use the ARM generic timer
 * virtual counter (CNTVCT_EL0 - the same counter the kernel tic is
 * derived from, 54MHz on BCM2712) for the command-gap / poll timing
 * below. The kernel opens EL0 counter access via CNTKCTL_EL1 on every
 * core (bsp/timer.c, bsp/core.c); the frequency comes from CNTFRQ_EL0
 * instead of a hardcoded constant.
 */
static inline uint32_t sdhci_now_us(void)
{
    static uint64_t cnt_us_div;
    uint64_t cnt;

    if (cnt_us_div == 0) {
        uint64_t frq;
        __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(frq));
        cnt_us_div = frq / 1000000UL;
        if (cnt_us_div == 0)
            cnt_us_div = 1;
    }
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(cnt));
    return (uint32_t)(cnt / cnt_us_div);
}

/* Call right before committing SDHCI_COMMAND; spins only the shortfall.
 * Bounded by iteration count so a stuck/unmapped 1MHz system timer cannot
 * wedge the entire SDIO stack in an infinite spin (silent hang + CPU burn).
 * 1M iterations is far more than enough for any real 250us-class gap. */
static void sdhci_pre_cmd_gap(uint32_t gap_us)
{
    uint32_t spins = 0;

    if (!sdhci_last_cmd_valid)
        return;
    while ((uint32_t)(sdhci_now_us() - sdhci_last_cmd_us) < gap_us) {
        if (++spins > 1000000)
            break;
    }
}

/*
 * Completion-wait policy for command/data polling loops.
 *
 * The waits in sdhci_send_command()/sdhci_transfer_data() are microsecond
 * scale: a command response arrives within ~64 SD clocks (~2us at 50MHz)
 * and a 512-byte block moves in ~20-90us. Yielding with sleep(0) on every
 * poll iteration hands the CPU to the scheduler for a full multi-ms round
 * trip (netd, sshd and the pump thread are all runnable during transfers),
 * so each CMD53 paid several milliseconds of pure scheduling latency --
 * ~5-8ms per WLAN data frame, capping TCP throughput near 200KB/s no
 * matter what the protocol stack did. Busy-spin for the expected-normal
 * window on the 1MHz system timer and only start yielding once the wait
 * is anomalously long (card stall / error paths), which preserves the
 * original don't-monopolise-the-CPU intent where it actually matters.
 */
#define SDHCI_POLL_SPIN_US 500

static inline void sdhci_poll_relax(uint32_t spin_start_us)
{
    if ((uint32_t)(sdhci_now_us() - spin_start_us) >= SDHCI_POLL_SPIN_US)
        sleep(0);
}


static void sdhci_set_power(struct sdhci_host *host, uint32_t power)
{
    u8 pwr = 0;
    switch (power) {
    case MMC_VDD_165_195:
        pwr = SDHCI_POWER_180;
        break;
    case MMC_VDD_29_30:
    case MMC_VDD_30_31:
        pwr = SDHCI_POWER_300;
        break;
    case MMC_VDD_32_33:
    case MMC_VDD_33_34:
        pwr = SDHCI_POWER_330;
        break;
    }

    if (pwr == 0) {
        sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);
        return;
    }

    pwr |= SDHCI_POWER_ON;

    sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);
}

int sdhci_set_clock(struct sdhci_host *host , unsigned int clock)
{
    unsigned int div, clk = 0, timeout;

    /* Wait max 20 ms */
    timeout = 200;
    while (sdhci_readl(host, SDHCI_PRESENT_STATE) &
               (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
        if (timeout == 0) {
            brcm_log("%s: Timeout to wait cmd & data inhibit\n",
                   __func__);
            return -EBUSY;
        }

        timeout--;
        usleep(1000);
    }

    sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

    if (clock == 0)
        return 0;

    if (SDHCI_GET_VERSION(host) >= SDHCI_SPEC_300) {
        /*
         * Check if the Host Controller supports Programmable Clock
         * Mode.
         */
        if (host->clk_mul) {
            for (div = 1; div <= 1024; div++) {
                if ((host->max_clk / div) <= clock)
                    break;
            }

            /*
             * Set Programmable Clock Mode in the Clock
             * Control register.
             */
            clk = SDHCI_PROG_CLOCK_MODE;
            div--;
        } else {
            /* Version 3.00 divisors must be a multiple of 2. */
            if (host->max_clk <= clock) {
                div = 1;
            } else {
                for (div = 2;
                     div < SDHCI_MAX_DIV_SPEC_300;
                     div += 2) {
                    if ((host->max_clk / div) <= clock)
                        break;
                }
            }
            /*
             * The 10-bit SDCLKFS field encodes N with
             * SDCLK = base / (2 * N) (N == 0 means base clock),
             * so the loop's real divisor has to be halved before
             * it is programmed - same as u-boot/Linux
             * sdhci_calc_clk(). Shifting the other way wrote 2*div
             * instead of div/2, i.e. every requested rate came out
             * 4x too slow (25MHz target -> 6.25MHz on the bus,
             * 400kHz identification -> 100kHz), which capped WLAN
             * throughput at a fraction of the bus capability.
             */
            div >>= 1;
        }
    } else {
        /* Version 2.00 divisors must be a power of 2. */
        for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
            if ((host->max_clk / div) <= clock)
                break;
        }
        div >>= 1;
    }


    clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
    clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
        << SDHCI_DIVIDER_HI_SHIFT;
    clk |= SDHCI_CLOCK_INT_EN;
    sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

    /* Wait max 20 ms */
    timeout = 20;
    while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
        & SDHCI_CLOCK_INT_STABLE)) {
        if (timeout == 0) {
            brcm_log("%s: Internal clock never stabilised.\n",
                   __func__);
            return -EBUSY;
        }
        timeout--;
        usleep(1000);
    }

    clk |= SDHCI_CLOCK_CARD_EN;
    sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

    /*
     * Log what the divisor really produced, not what was asked for: a
     * mis-encoded SDCLKFS field is invisible from the outside and shows up
     * only as unexplained low throughput.
     */
    brcm_log("sdhci: clock req %uHz base %uHz div %u -> %uHz\n",
            clock, host->max_clk, div,
            host->clk_mul ? (host->max_clk / (div + 1)) :
            (div ? (host->max_clk / (2 * div)) : host->max_clk));
    return 0;
}

static void sdhci_set_bus_width(struct sdhci_host *host, int bus_width){
    /* Set bus width */
    uint8_t ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
    if (bus_width == 8) {
        ctrl &= ~SDHCI_CTRL_4BITBUS;
        if ((SDHCI_GET_VERSION(host) >= SDHCI_SPEC_300) ||
                (host->quirks & SDHCI_QUIRK_USE_WIDE8))
            ctrl |= SDHCI_CTRL_8BITBUS;
    } else {
        if ((SDHCI_GET_VERSION(host) >= SDHCI_SPEC_300) ||
                (host->quirks & SDHCI_QUIRK_USE_WIDE8))
            ctrl &= ~SDHCI_CTRL_8BITBUS;
        if (bus_width == 4)
            ctrl |= SDHCI_CTRL_4BITBUS;
        else
            ctrl &= ~SDHCI_CTRL_4BITBUS;
    }
    sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

static void sdhci_set_select_mode(struct sdhci_host *host, int mode){
    bool no_hispd_bit = false;
    /* Set bus width */
    uint8_t ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);

    if ((host->quirks & SDHCI_QUIRK_NO_HISPD_BIT) ||
        (host->quirks & SDHCI_QUIRK_BROKEN_HISPD_MODE)) {
        ctrl &= ~SDHCI_CTRL_HISPD;
        no_hispd_bit = true;
    }

    if (!no_hispd_bit) {
        if (mode == MMC_HS ||
            mode == SD_HS ||
            mode == MMC_HS_52 ||
            mode == MMC_DDR_52 ||
            mode == MMC_HS_200 ||
            mode == MMC_HS_400 ||
            mode == MMC_HS_400_ES ||
            mode == UHS_SDR25 ||
            mode == UHS_SDR50 ||
            mode == UHS_SDR104 ||
            mode == UHS_DDR50)
            ctrl |= SDHCI_CTRL_HISPD;
        else
            ctrl &= ~SDHCI_CTRL_HISPD;
    }
    sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

static int sdhci_get_info(struct sdhci_host *host)
{
    u32 caps, caps_1 = 0;
    caps = sdhci_readl(host, SDHCI_CAPABILITIES);
    host->version = sdhci_readw(host, SDHCI_HOST_VERSION);

    /*
     * Force SDMA regardless of the advertised capability bit. BCM2712
     * SDIO2 streams CMD53 payloads correctly through the 32-bit
     * system-address SDMA path (the bounce pool is <4GB and mapped
     * uncached, so no cache flush is required), but it does not reliably
     * set SDHCI_CAN_DO_SDMA, which would otherwise leave the hot path on
     * the slow PIO FIFO copy. If the controller ever rejects a DMA
     * transfer, sdhci_transfer_data_sdma() degrades back to PIO after
     * SDHCI_SDMA_FAIL_LIMIT consecutive failures, so the link stays
     * functional either way.
     */
    host->flags |= USE_SDMA;
    brcm_log("sdhci: caps=0x%08x can_do_sdma=%d, USE_SDMA forced on\n",
            caps, (caps & SDHCI_CAN_DO_SDMA) ? 1 : 0);

    /* Check whether the clock multiplier is supported or not */
    if (SDHCI_GET_VERSION(host) >= SDHCI_SPEC_300) {
        caps_1 = sdhci_readl(host, SDHCI_CAPABILITIES_1);
        host->clk_mul = (caps_1 & SDHCI_CLOCK_MUL_MASK) >>
                SDHCI_CLOCK_MUL_SHIFT;
        host->host_caps |= (caps_1 & SDHCI_SUPPORT_DDR50) ?
                MMC_CAP_UHS_DDR50 : 0;
    }

    if (host->max_clk == 0) {
        if (SDHCI_GET_VERSION(host) >= SDHCI_SPEC_300)
            host->max_clk = (caps & SDHCI_CLOCK_V3_BASE_MASK) >>
                SDHCI_CLOCK_BASE_SHIFT;
        else
            host->max_clk = (caps & SDHCI_CLOCK_BASE_MASK) >>
                SDHCI_CLOCK_BASE_SHIFT;
        host->max_clk *= 1000000;
        if (host->clk_mul)
            host->max_clk *= host->clk_mul;
    }
    if (host->max_clk == 0) {
        brcm_log("%s: Hardware doesn't specify base clock frequency\n",
               __func__);
        return -EINVAL;
    }

    return 0;
}


static void sdhci_set_uhs_timing(struct sdhci_host *host, uint32_t mode)
{
    u32 reg;

    reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
    reg &= ~SDHCI_CTRL_UHS_MASK;

    switch (mode) {
    case UHS_SDR25:
    case MMC_HS:
        reg |= SDHCI_CTRL_UHS_SDR25;
        break;
    case UHS_SDR50:
    case MMC_HS_52:
        reg |= SDHCI_CTRL_UHS_SDR50;
        break;
    case UHS_DDR50:
    case MMC_DDR_52:
        reg |= SDHCI_CTRL_UHS_DDR50;
        break;
    case UHS_SDR104:
    case MMC_HS_200:
        reg |= SDHCI_CTRL_UHS_SDR104;
        break;
    case MMC_HS_400:
    case MMC_HS_400_ES:
        reg |= SDHCI_CTRL_HS400;
        break;
    default:
        reg |= SDHCI_CTRL_UHS_SDR12;
    }

    sdhci_writew(host, reg, SDHCI_HOST_CONTROL2);
}


/****************************************************************************/

void sdhci_reset(u8 mask)
{
    unsigned long timeout;

    /* Wait max 100 ms */
    timeout = 100;
    sdhci_writeb(&_host, mask, SDHCI_SOFTWARE_RESET);
    while (sdhci_readb(&_host, SDHCI_SOFTWARE_RESET) & mask) {
        if (timeout == 0) {
            brcm_log("%s: Reset 0x%x never completed.\n",
                   __func__, (int)mask);
            return;
        }
        timeout--;
        usleep(1000);
    }
}

void sdhci_enable_irq(int enable)
{
    uint32_t ier = sdhci_readl(&_host, SDHCI_INT_ENABLE);
    if (enable)
        ier |= SDHCI_INT_CARD_INT;
    else
        ier &= ~SDHCI_INT_CARD_INT;

    sdhci_writel(&_host, ier, SDHCI_INT_ENABLE);
    sdhci_writel(&_host, ier, SDHCI_SIGNAL_ENABLE);
}


static void sdhci_transfer_pio(struct sdhci_host *host, struct mmc_data *data)
{
    uint32_t i;
    uint8_t *offs;
    for (i = 0; i < data->blocksize; i += 4) {
        uint32_t val;
        uint32_t remain = data->blocksize - i;

        offs = (data->flags == MMC_DATA_READ) ?
            (data->dest + i) : (data->src + i);
        if (data->flags == MMC_DATA_READ) {
            val = sdhci_readl(host, SDHCI_BUFFER);
            if (remain >= 4)
                memcpy(offs, &val, 4);
            else
                memcpy(offs, &val, remain);
        } else {
            val = 0;
            if (remain >= 4)
                memcpy(&val, offs, 4);
            else
                memcpy(&val, offs, remain);
            sdhci_writel(host, val, SDHCI_BUFFER);
        }
    }
}

static int sdhci_transfer_data(struct sdhci_host *host, struct mmc_data *data)
{
    unsigned int stat, rdy, mask, block = 0;
    bool transfer_done = false;
    uint32_t spin_start_us = sdhci_now_us();
    /* Wall-clock deadline, not an iteration count: a stuck FIFO used to
     * cost 100000 sleep(0) round trips (tens of seconds under load) and
     * wedged the whole firmware download. Measured as elapsed-since-start:
     * get_timer() is an unsigned kernel_tic_ms delta, so the old
     * "now + timeout" deadline underflowed and fired on the very first
     * poll (every chip-attach CMD53 "timed out" within microseconds). */
    uint64_t xfer_start = get_timer(0);
    if (data->flags == MMC_DATA_READ) {
        rdy = SDHCI_INT_DATA_AVAIL;
        mask = SDHCI_DATA_AVAILABLE;
    } else {
        rdy = SDHCI_INT_SPACE_AVAIL;
        mask = SDHCI_SPACE_AVAILABLE;
    }
    do {
        stat = sdhci_readl(host, SDHCI_INT_STATUS);
        if (stat & SDHCI_INT_ERROR) {
            brcm_log("%s: Error detected in status(0x%X)!\n",
                 __func__, stat);
            return -EIO;
        }
        if (!transfer_done && (stat & rdy)) {
            if (!(sdhci_readl(host, SDHCI_PRESENT_STATE) & mask))
                continue;
            sdhci_writel(host, stat & rdy, SDHCI_INT_STATUS);
            sdhci_transfer_pio(host, data);
            /*
             * The spin/yield budget is for one FIFO-ready wait, not
             * the whole multi-block CMD53. A normal 3-block WLAN
             * frame can exceed SDHCI_POLL_SPIN_US end-to-end, so
             * keeping the transfer-start timestamp here drops the
             * second/third block straight into sleep(0) even though
             * the controller is making steady progress - that
             * injects a millisecond-class scheduler gap into every
             * packet and caps throughput in the few-hundred-KB/s
             * range. Progress just happened, so start a fresh spin
             * window for the next FIFO-ready phase.
             */
            spin_start_us = sdhci_now_us();
            if (data->flags == MMC_DATA_READ)
                data->dest += data->blocksize;
            else
                data->src += data->blocksize;
            if (++block >= data->blocks) {
                /* Keep looping until the SDHCI_INT_DATA_END is
                 * cleared, even if we finished sending all the
                 * blocks.
                 */
                transfer_done = true;
                continue;
            }
        } else {
            /* FIFO ready is ~20-90us away in the normal case; spin
             * first, yield only on anomalously long waits (see
             * sdhci_poll_relax).
             */
            sdhci_poll_relax(spin_start_us);
        }
        if (get_timer(xfer_start) >= SDHCI_DATA_TIMEOUT_MS){
            brcm_log("%s: Transfer data timeout\n", __func__);
            return -ETIMEDOUT;
        }
    } while (!(stat & SDHCI_INT_DATA_END));

    return 0;
}

static int sdhci_transfer_data_sdma(struct sdhci_host *host,
        struct mmc_data *data)
{
    unsigned int stat;
    uint64_t start = get_timer(0);

    while (1) {
        stat = sdhci_readl(host, SDHCI_INT_STATUS);
        if (stat & SDHCI_INT_ERROR) {
            if (++_sdma_fail_streak >= SDHCI_SDMA_FAIL_LIMIT) {
                host->flags &= ~USE_SDMA;
                brcm_log("%s: SDMA error status=0x%x, %u consecutive fails -> PIO fallback\n",
                        __func__, stat, _sdma_fail_streak);
            } else {
                brcm_log("%s: SDMA error status=0x%x (streak %u), retrying\n",
                        __func__, stat, _sdma_fail_streak);
            }
            return -EIO;
        }
        if (stat & SDHCI_INT_DMA_END) {
            sdhci_writel(host, sdhci_readl(host, SDHCI_DMA_ADDRESS),
                    SDHCI_DMA_ADDRESS);
            sdhci_writel(host, SDHCI_INT_DMA_END, SDHCI_INT_STATUS);
        }
        if (stat & SDHCI_INT_DATA_END)
            break;
        if (get_timer(start) >= SDHCI_SDMA_TIMEOUT_MS) {
            if (++_sdma_fail_streak >= SDHCI_SDMA_FAIL_LIMIT) {
                host->flags &= ~USE_SDMA;
                brcm_log("%s: SDMA timeout status=0x%x, %u consecutive fails -> PIO fallback\n",
                        __func__, stat, _sdma_fail_streak);
            } else {
                brcm_log("%s: SDMA timeout status=0x%x (streak %u), retrying\n",
                        __func__, stat, _sdma_fail_streak);
            }
            return -ETIMEDOUT;
        }
    }

    /* Full DMA burst completed cleanly: clear any transient-failure streak. */
    _sdma_fail_streak = 0;
    if (data->flags == MMC_DATA_READ) {
        memcpy(data->dest, _sdma_bounce,
                data->blocks * data->blocksize);
    }
    return 0;
}

static void sdhci_cmd_done(struct sdhci_host *host, struct mmc_cmd *cmd)
{
    int i;
    if (cmd->resp_type & MMC_RSP_136) {
        /* CRC is stripped so we need to do some shifting. */
        for (i = 0; i < 4; i++) {
            cmd->response[i] = sdhci_readl(host,
                    SDHCI_RESPONSE + (3-i)*4) << 8;
            if (i != 3)
                cmd->response[i] |= sdhci_readb(host,
                        SDHCI_RESPONSE + (3-i)*4-1);
        }
    } else {
        cmd->response[0] = sdhci_readl(host, SDHCI_RESPONSE);
    }
}

int sdhci_send_command(struct mmc_cmd *cmd, struct mmc_data *data)
{
    struct sdhci_host *host = &_host;
    unsigned int stat = 0;
    int ret = 0;
    int trans_bytes = 0, is_aligned = 1;
    u32 mask, flags, mode = 0;
    uint64_t start = get_timer(0);
    bool use_sdma = false;

    host->start_addr = 0;

    mask = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;

    /* We shouldn't wait for data inihibit for stop commands, even
       though they might use busy signaling */
    if (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION ||
        ((cmd->cmdidx == MMC_CMD_SEND_TUNING_BLOCK ||
          cmd->cmdidx == MMC_CMD_SEND_TUNING_BLOCK_HS200) && !data))
        mask &= ~SDHCI_DATA_INHIBIT;

    {
        /* Wall-clock busy cap: the old iteration-count loop (each
         * iteration one sleep(0) round trip, doubling up to 32000)
         * turned one stuck inhibit bit into tens of seconds of
         * scheduler spinning before anyone noticed. Elapsed-since-
         * start form: get_timer() deltas are unsigned, a "now +
         * timeout" deadline underflows and fires immediately. */
        uint64_t busy_start = get_timer(0);

        while (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask) {
            if (get_timer(busy_start) >= SDHCI_CMD_MAX_TIMEOUT) {
                brcm_log("%s: busy timeout\n", __func__);
                return -ECOMM;
            }
            usleep(1000);
        }
    }

    sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

    mask = SDHCI_INT_RESPONSE;
    if ((cmd->cmdidx == MMC_CMD_SEND_TUNING_BLOCK ||
         cmd->cmdidx == MMC_CMD_SEND_TUNING_BLOCK_HS200) && !data)
        mask = SDHCI_INT_DATA_AVAIL;

    if (!(cmd->resp_type & MMC_RSP_PRESENT))
        flags = SDHCI_CMD_RESP_NONE;
    else if (cmd->resp_type & MMC_RSP_136)
        flags = SDHCI_CMD_RESP_LONG;
    else if (cmd->resp_type & MMC_RSP_BUSY) {
        flags = SDHCI_CMD_RESP_SHORT_BUSY;
        mask |= SDHCI_INT_DATA_END;
    } else
        flags = SDHCI_CMD_RESP_SHORT;

    if (cmd->resp_type & MMC_RSP_CRC)
        flags |= SDHCI_CMD_CRC;
    if (cmd->resp_type & MMC_RSP_OPCODE)
        flags |= SDHCI_CMD_INDEX;
    if (data || cmd->cmdidx ==  MMC_CMD_SEND_TUNING_BLOCK ||
        cmd->cmdidx == MMC_CMD_SEND_TUNING_BLOCK_HS200)
        flags |= SDHCI_CMD_DATA;

    /* Set Transfer mode regarding to data flag */
    if (data) {
        sdhci_writeb(host, 0xe, SDHCI_TIMEOUT_CONTROL);

        mode = SDHCI_TRNS_BLK_CNT_EN;
        trans_bytes = data->blocks * data->blocksize;
        if (data->blocks > 1)
            mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN;

        if (data->flags == MMC_DATA_READ)
            mode |= SDHCI_TRNS_READ;

        if ((host->flags & USE_SDMA) &&
                trans_bytes <= SDHCI_SDMA_BOUNCE_SIZE &&
                sdhci_sdma_init() == 0) {
            use_sdma = true;
            mode |= SDHCI_TRNS_DMA;
            if (data->flags != MMC_DATA_READ)
                memcpy(_sdma_bounce, data->src, trans_bytes);
            sdhci_writel(host, _sdma_bounce_phys, SDHCI_DMA_ADDRESS);
        }

        if (!_xfer_path_logged) {
            _xfer_path_logged = true;
            brcm_log("sdhci: first data xfer path=%s (flags=0x%x trans_bytes=%d)\n",
                    use_sdma ? "SDMA" : "PIO",
                    host->flags, trans_bytes);
        }

        sdhci_writew(host, SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG,
                data->blocksize),
                SDHCI_BLOCK_SIZE);
        sdhci_writew(host, data->blocks, SDHCI_BLOCK_COUNT);
        sdhci_writew(host, mode, SDHCI_TRANSFER_MODE);
    } else if (cmd->resp_type & MMC_RSP_BUSY) {
        sdhci_writeb(host, 0xe, SDHCI_TIMEOUT_CONTROL);
    }

    sdhci_writel(host, cmd->cmdarg, SDHCI_ARGUMENT);
    if (host->quirks & SDHCI_QUIRK_WAIT_SEND_CMD) {
        /* CMD53 to F2 (data FIFO) tolerates a shorter gap than the
         * control-plane CMD52/F1 accesses; see gap constants above. */
        uint32_t gap_us = SDHCI_MIN_CMD_GAP_US;
        if (cmd->cmdidx == 53 && ((cmd->cmdarg >> 28) & 0x7) == 2)
            gap_us = SDHCI_MIN_CMD_GAP_F2_US;
        sdhci_pre_cmd_gap(gap_us);
    }
    sdhci_writew(host, SDHCI_MAKE_CMD(cmd->cmdidx, flags), SDHCI_COMMAND);
    start = get_timer(0);
    {
        uint32_t cmd_spin_start_us = sdhci_now_us();
        do {
            stat = sdhci_readl(host, SDHCI_INT_STATUS);
            if (stat & SDHCI_INT_ERROR)
                break;

            if (host->quirks & SDHCI_QUIRK_BROKEN_R1B &&
                cmd->resp_type & MMC_RSP_BUSY && !data) {
                unsigned int state =
                    sdhci_readl(host, SDHCI_PRESENT_STATE);

                if (!(state & SDHCI_DAT_ACTIVE))
                    return 0;
            }

            if (get_timer(start) >= SDHCI_READ_STATUS_TIMEOUT) {
                brcm_log("%s: Timeout for status update: %08x %08x\n",
                       __func__, stat, mask);
                return -ETIMEDOUT;
            }
            /* Response lands within ~64 SD clocks; spin first,
             * yield only if the wait turns anomalous. */
            sdhci_poll_relax(cmd_spin_start_us);
        } while ((stat & mask) != mask);
    }

    if ((stat & (SDHCI_INT_ERROR | mask)) == mask) {
        sdhci_cmd_done(host, cmd);
        sdhci_writel(host, mask, SDHCI_INT_STATUS);
    } else
        ret = -1;

    if (!ret && data) {
        if (use_sdma)
            ret = sdhci_transfer_data_sdma(host, data);
        else
            ret = sdhci_transfer_data(host, data);
    }

    if (host->quirks & SDHCI_QUIRK_WAIT_SEND_CMD) {
        sdhci_last_cmd_us = sdhci_now_us();
        sdhci_last_cmd_valid = 1;
    }

    stat = sdhci_readl(host, SDHCI_INT_STATUS);
    sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

    // if(cmd->cmdidx != 52 && cmd->cmdidx != 53)
    // 	brcm_log("ret:%d resp: %x %x %x %x\n", ret, cmd->response[0], cmd->response[1],cmd->response[2],cmd->response[3]);

    if (!ret) {
        if ((host->quirks & SDHCI_QUIRK_32BIT_DMA_ADDR) &&
                !is_aligned && (data->flags == MMC_DATA_READ))
            memcpy(data->dest, host->align_buffer, trans_bytes);
        return 0;
    }

    sdhci_reset(SDHCI_RESET_CMD);
    sdhci_reset(SDHCI_RESET_DATA);
    
    if (stat & SDHCI_INT_TIMEOUT)
        return -ETIMEDOUT;
    else
        return -ECOMM;
}

int sdhci_set_ios(struct mmc *mmc)
{
    struct sdhci_host *host = &_host;

        host->mmc = mmc;

    if (mmc->clock != host->clock)
        sdhci_set_clock(host, mmc->clock);
        host->clock = mmc->clock;

    if (mmc->clk_disable)
        sdhci_set_clock(host, 0);

    sdhci_set_bus_width(host, mmc->bus_width);
        host->bus_width = mmc->bus_width;
    sdhci_set_select_mode(host, mmc->selected_mode);
        sdhci_set_uhs_timing(host, mmc->selected_mode);
    return 0;
}

void sdhci_init(void)
{
    bcm2712_sdhci_gpio_init();

    _host.bus_width  = 1;
    /* Prefer the firmware-reported clk_emmc2; 200MHz is the dts value. */
    _host.max_clk = _sdhci_base_clk ? _sdhci_base_clk : 200000000;
    _host.clock = 400000;
    _host.name = "sdhci";
    /*
     * Onboard CYW43455 hangs off the sdio2 host ("brcm,bcm2712-sdhci")
     * at 0x10_0110_0000; the SD card slot uses the separate sdio1 host.
     * The window is mapped by pi5_platform_map() before this runs.
     */
    _host.ioaddr = (void*)(_mmio_base + PI5_EMMC_WIN_OFF + PI5_WLAN_SDIO_OFF);
    _host.twoticks_delay = ((2 * 1000000) / 400000) + 1;
    _host.last_write = 0;
        /*
         * Keep the same proven quirk set as the bcm2712 arch SDHCI path.
         * The Pi5 WLAN userspace host still does 8/16-bit split register
         * accesses and PIO command pacing through this wrapper; dropping the
         * legacy BROKEN_* / NO_HISPD_BIT flags made fn1/backplane accesses
         * fail even after identification succeeded.
         */
        _host.quirks = SDHCI_QUIRK_BROKEN_VOLTAGE |
                       SDHCI_QUIRK_BROKEN_R1B |
                       SDHCI_QUIRK_WAIT_SEND_CMD |
                       SDHCI_QUIRK_NO_HISPD_BIT;
    _host.voltages = MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;

    /* non-removable chip: force card presence in the vendor cfg regs */
    pi5_platform_force_card_present();

    sdhci_reset(SDHCI_RESET_ALL);
    sdhci_set_power(&_host,MMC_VDD_33_34);

    sdhci_get_info(&_host);
    if (_host.flags & USE_SDMA) {
        uint8_t ctrl = sdhci_readb(&_host, SDHCI_HOST_CONTROL);

        ctrl &= ~SDHCI_CTRL_DMA_MASK;
        ctrl |= SDHCI_CTRL_SDMA;
        sdhci_writeb(&_host, ctrl, SDHCI_HOST_CONTROL);
    }

    /*
     * Keep the bus in identification mode here. The card-side bus width
     * switch happens later through CCCR_IF after CMD7 succeeds.
     */
    sdhci_set_clock(&_host, 400000);
    sdhci_set_bus_width(&_host, 1);
    sdhci_set_uhs_timing(&_host, 0);
    /* Enable only interrupts served by the SD controller */
    sdhci_writel(&_host, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK,
            SDHCI_INT_ENABLE);

    /* Mask all sdhci interrupt sources */
    sdhci_writel(&_host, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK,
            SDHCI_SIGNAL_ENABLE);
    return;
}
