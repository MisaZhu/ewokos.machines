#ifndef ORIN_BSP_WDT_H
#define ORIN_BSP_WDT_H

#include <stdint.h>

/*
 * TEST-ONLY SBSA Generic Watchdog (Tegra234 tegra-wdt-t234).  NOT for
 * mainline; used by the WATCHDOG_TEST bring-up image.  Single-stage mode:
 * WS0 interrupt ignored, WS1 resets the SoC at the programmed timeout.
 * See include/orin_net.h for bases; upstream sbsa_gwdt.c for the protocol.
 */

int bsp_wdt_init(void);
int bsp_wdt_start(uint32_t timeout_sec);  /* program + enable */
void bsp_wdt_pet(void);                    /* refresh (write WRR) */
void bsp_wdt_stop(void);
int bsp_wdt_enabled(void);
int bsp_wdt_reset_caused(void);            /* WCS.WS1 set after a WDT reset */

#endif