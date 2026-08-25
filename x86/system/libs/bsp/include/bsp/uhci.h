/*
 * uhci.h: polled UHCI host controller driver (PCI, I/O-bar based).
 *
 * The bsp_usb layer talks to the hardware only through this interface.
 * Everything is synchronous/polled: transfers build a TD chain, hook it
 * into the controller's single async queue head and spin on the TD
 * status bits. There is no interrupt schedule and no periodic list.
 *
 * Root ports are exposed as one flat 0-based space laid out as
 * (present controller index * UHCI_PORTS_PER_CTRL + port), matching the
 * 1-based flattening the bsp layer presents to the policy layer.
 */
#ifndef __UHCI_H__
#define __UHCI_H__

#include <stdint.h>
#include <stdbool.h>
#include <usb/usb_defs.h>

#define UHCI_MAX_CONTROLLERS 4
#define UHCI_PORTS_PER_CTRL  2

/* dma pool + PCI probe + controller bring-up; 0 = usable (possibly with
   zero controllers, callers then just see zero ports) */
int  uhci_init(void);

/* full controller re-bring-up after a wedged enumeration; drops all
   transfer state. 0 = ok, -1 = nothing to re-init */
int  uhci_reinit(void);

/* flat root port space across the present controllers */
int  uhci_port_count(void);
bool uhci_port_connected(int flat_port);
/* reset+enable; returns BSP_USB_SPEED_LOW/FULL style codes:
   0 low speed, 1 full speed, < 0 failure */
int  uhci_reset_port(int flat_port);
/* clear the connect/enable change latches (W1C) */
void uhci_ack_port_change(int flat_port);
/* re-enable a port that dropped PE after a transfer error */
void uhci_recover_port(int flat_port);

/* control transfer on EP0: returns bytes moved or < 0 */
int  uhci_control_xfer(int flat_port, bool low_speed, uint8_t addr,
        uint8_t ep_mps, const usb_setup_pkt_t* setup, void* data,
        bool dir_in);

/* interrupt-IN poll: >0 data bytes (toggle advanced), 0 no data yet,
   -2 endpoint stalled, -1 hard error */
int  uhci_int_in_xfer(int flat_port, bool low_speed, uint8_t addr,
        uint8_t ep, uint16_t mps, uint8_t* toggle, void* data,
        uint16_t size);

/* bulk transfer with an internal NAK retry loop (flash devices NAK
   while programming): returns bytes moved, -2 stalled, -1 error */
int  uhci_bulk_xfer(int flat_port, bool low_speed, bool dir_in,
        uint8_t addr, uint8_t ep, uint16_t mps, uint8_t* toggle,
        void* data, uint32_t len, uint32_t timeout_ms);

#endif /* __UHCI_H__ */
