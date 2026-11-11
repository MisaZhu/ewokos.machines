/*
 * adreno_wdt.c - arm the APSS watchdog around the CM3 release.
 *
 * The Adreno bring-up can wedge the whole SoC at the moment GMU_CM3_SYSRESET
 * is deasserted: no serial, no network, and the EUD SRST path cannot recover
 * it (the board ends up in EDL).  The kernel deliberately disables the APSS
 * watchdog at boot (q8b_early_watchdog_disable), so a hung test needs a
 * manual power cycle.
 *
 * ADRENO_ARM_WDT=1 re-arms it just before the release and disables it again
 * if the release survives.  A hang then self-recovers after ~10 s, which is
 * what makes the ssh+serial test loop unattended.
 *
 * Layout (drivers/watchdog/qcom-wdt.c, kpss variant, base 0x17c10000 from
 * sc8280xp.dtsi apss_wdt): RST 0x04, EN 0x08, STS 0x0c, BARK 0x10,
 * BITE 0x14, rate 32768 Hz.  BITE resets the SoC.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <syscalls.h>

#define WDT_PHY   0x17c10000ULL
#define WDT_SIZE  0x00001000u
#define WDT_VA    (ADRENO_MMIO_VA_BASE + 0x00500000ULL)
#define WDT_RST   0x04u
#define WDT_EN    0x08u
#define WDT_BARK  0x10u
#define WDT_BITE  0x14u
#define WDT_RATE  32768u

static int _wdt_mapped;

static int wdt_map(void)
{
	if (_wdt_mapped)
		return 0;
	if (syscall3(SYS_MEM_MAP, WDT_VA, WDT_PHY, (ewokos_addr_t)WDT_SIZE) != WDT_VA)
		return -1;
	_wdt_mapped = 1;
	return 0;
}

int adreno_wdt_arm(unsigned seconds)
{
	uint32_t bite, bark;

	if (wdt_map() != 0)
		return -1;
	if (seconds == 0)
		seconds = 10;
	bite = seconds * WDT_RATE;
	bark = (seconds > 3 ? seconds - 3 : 1) * WDT_RATE;

	put32(WDT_VA + WDT_EN, 0);
	put32(WDT_VA + WDT_RST, 1);
	put32(WDT_VA + WDT_BARK, bark);
	put32(WDT_VA + WDT_BITE, bite);
	put32(WDT_VA + WDT_EN, 1);
	klog("adreno_wdt: armed %us (bite=%u)\n", seconds, bite);
	return 0;
}

void adreno_wdt_disable(void)
{
	if (_wdt_mapped)
		put32(WDT_VA + WDT_EN, 0);
}
