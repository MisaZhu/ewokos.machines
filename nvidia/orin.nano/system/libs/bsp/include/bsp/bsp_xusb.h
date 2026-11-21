#ifndef ORIN_BSP_XUSB_H
#define ORIN_BSP_XUSB_H

#include <stdint.h>

/*
 * Tegra234 xHCI host controller bring-up (usb@3610000).  Powers/clocks the
 * XUSB block via BPMP, configures the USB2 padctl PHY for host mode, maps
 * the FPCI BARs, and waits for the xHCI core to leave reset.  Returns the
 * virtual address of the xHCI capability registers so a standard xHCI host
 * driver (e.g. the shared/qcom xhci core) can take over with xhci_init().
 *
 * This is the controller bring-up layer only; the xHCI ring/endpoint driver
 * and USB device classes live above it.  USB2-only host (the devkit's 4 USB-A
 * ports sit behind a hub on usb2-1); SS/UPHY ELPG is a follow-up.
 */

/* Returns xHCI capability-base VA, or 0 on failure. */
uint64_t bsp_xusb_init(void);

/* Read a root-port PORTSC (1-based port).  Returns 0xffffffff if not up. */
uint32_t bsp_xusb_portsc(int port);

#endif