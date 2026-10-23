#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sysinfo.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>

/*
 * BCM2712 (Raspberry Pi 5) reboot through the SoC PM/watchdog block.
 *
 * The PM block lives at physical 0x10_7D200000 (dtb node
 * soc@107c000000/watchdog@7d200000, compatible "brcm,bcm2712-pm",
 * system-power-controller), which is inside the main 64MB MMIO window
 * (0x10_7C000000) that mmio_map() already provides, so no extra mapping
 * is needed -- PI5_PM_OFF is just the block's offset inside that window.
 *
 * This is the same watchdog the uConsole powerd uses to power the CM5 off;
 * the only difference is the PM_RSTS boot partition: writing partition 0
 * (instead of the halt partition 63) makes the VPU firmware restart rather
 * than switch off the module rails.  Arming PM_WDOG and then writing
 * PM_RSTC with the full-reset config fires the reset ~150us later.
 */
#define PI5_PM_OFF               0x1200000  /* 0x107D200000 - 0x107C000000 */
#define PM_RSTC                  0x1c
#define PM_RSTS                  0x20
#define PM_WDOG                  0x24
#define PM_PASSWORD              0x5a000000
#define PM_RSTC_WRCFG_CLR        0xffffffcf
#define PM_RSTC_WRCFG_FULL_RESET 0x20
#define PM_RSTS_PARTITION_CLR    0xfffffaaa
#define PM_RSTS_PARTITION_BOOT   0x000      /* partition 0 = normal reboot */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    setbuf(stdout, NULL);

    _mmio_base = mmio_map();
    sys_info_t sysinfo;
    sys_get_sys_info(&sysinfo);

    /* The PM block must sit inside the mapped MMIO window; refuse rather
     * than write through an unmapped offset and take a fault. */
    ewokos_addr_t pm_phy = sysinfo.mmio.phy_base + (ewokos_addr_t)PI5_PM_OFF;
    if(pm_phy < sysinfo.mmio.phy_base ||
       pm_phy + 0x100 > sysinfo.mmio.phy_base + sysinfo.mmio.size) {
        printf("reboot: PM block 0x%016llX outside MMIO window "
               "[0x%016llX - 0x%016llX]\n",
               (unsigned long long)pm_phy,
               (unsigned long long)sysinfo.mmio.phy_base,
               (unsigned long long)(sysinfo.mmio.phy_base + sysinfo.mmio.size));
        return -1;
    }

    volatile uint32_t* rstc = (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_RSTC);
    volatile uint32_t* rsts = (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_RSTS);
    volatile uint32_t* wdog = (volatile uint32_t*)(_mmio_base + PI5_PM_OFF + PM_WDOG);

    /* Select boot partition 0 so the firmware restarts instead of halting. */
    uint32_t val = *rsts;
    val &= PM_RSTS_PARTITION_CLR;
    val |= PM_PASSWORD | PM_RSTS_PARTITION_BOOT;
    *rsts = val;

    /* Arm the watchdog (~150us) then trigger a full SoC reset. */
    *wdog = PM_PASSWORD | 10;

    val = *rstc;
    val &= PM_RSTC_WRCFG_CLR;
    val |= PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
    *rstc = val;

    printf("rebooting...\n");

    /* The reset fires shortly; spin until the board drops. */
    for(;;)
        ;

    return 0;
}
