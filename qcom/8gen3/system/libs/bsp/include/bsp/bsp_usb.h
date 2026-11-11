/*
 * bsp_usb.h: x86-local include path shim.
 *
 * The platform USB host abstraction contract is the SHARED header
 * system/gui/libs/usb/include/usb/bsp_usb.h (one header for every
 * platform, shipped with libusb); this file only keeps the historical
 * <bsp/bsp_usb.h> include path used by the x86 bsp sources working.
 * Add new contract entries to the shared header, never here.
 */
#ifndef __BSP_USB_LOCAL_SHIM_H__
#define __BSP_USB_LOCAL_SHIM_H__

#include <usb/bsp_usb.h>

/* x86-internal glue between bsp_usb.c (controller layer) and
   bsp_usbbt.c (Intel BT HCI service) - not part of the shared contract */
bool bsp_usb_xhci_owned(bsp_usb_dev_t* dev);
void bsp_usb_bt_poll(void);
void bsp_usb_bt_reset(void);

#endif /* __BSP_USB_LOCAL_SHIM_H__ */
