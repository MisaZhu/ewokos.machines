#ifndef __WLAN_PLATFORM_H__
#define __WLAN_PLATFORM_H__

#include <stdint.h>
#include <stdbool.h>

/*
 * Raspberry Pi 5 (BCM2712) WiFi platform glue.
 *
 * The radio itself is the same CYW43455 combo chip as on Pi4 (802.11ac,
 * 2.4/5GHz, BT5.0 BLE); everything Pi5-specific is the SDIO bus it hangs
 * off. From the official device tree (bcm2712.dtsi / bcm2712-rpi-5-b.dts):
 *
 *   - WiFi SDIO host: sdio2, "brcm,bcm2712-sdhci" @ 0x10_0110_0000,
 *     vendor cfg regs at +0x400, base clock clk_emmc2 = 200MHz
 *   - SDIO pins: BCM2712 GPIO30(clk)/31(cmd)/32-35(dat), function "sd2"
 *     (fsel 1 on D0 stepping) via pinctrl@7d504100
 *   - WL_REG_ON: "gio" brcmstb-gpio @ 0x7d508500, GPIO28 active-high,
 *     fixed 3.3V regulator with 150ms startup delay
 *   - no external 32kHz clock (Pi4's GPCLK2 scheme does not exist here)
 */

/* mux the six SDIO pins to sd2 and set dts pulls (clk none, cmd/dat up) */
void pi5_platform_pins(void);

/* drive WL_REG_ON (gio GPIO28) */
void pi5_platform_reg_on(bool on);

/* map the WiFi SDIO window into this process; 0 on success */
int pi5_platform_map(void);

/* sdio2 is non-removable: force card presence in the vendor cfg regs
 * (SDIO_CFG_CTRL, same as sdhci-brcmstb cfginit_2712). */
void pi5_platform_force_card_present(void);

#endif
