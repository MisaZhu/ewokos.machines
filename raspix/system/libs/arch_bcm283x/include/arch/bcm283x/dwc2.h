#ifndef __ARCH_BCM283X_DWC2_H
#define __ARCH_BCM283X_DWC2_H

/*
 * Polled DWC2 (Synopsys DesignWare USB2 OTG) host controller driver for
 * BCM283x/BCM2711: the single root port that raspix exposes to the bsp
 * usb layer.
 *
 * Everything is polled and internally serialized: the daemon's main loop
 * (HID interrupt-IN polling, enumeration control transfers) and the IPC
 * thread (mass-storage bulk traffic) may call in concurrently, each
 * top-level transfer takes the driver lock around its channel/DMA-pool
 * usage.
 *
 * Channel layout: 0 = control (EP0), 1 = interrupt-IN (HID), 2/3 =
 * bulk out/in (mass storage). No split transactions: FS/LS devices
 * behind a high-speed hub cannot be served, and a board whose 480Mbps
 * data path is dead gets latched into FS/LS-only mode via
 * dwc2_force_fs_only().
 */

#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/ewokdef.h>
#include <usb/usb_defs.h>

/* port speeds returned by dwc2_reset_port() (match BSP_USB_SPEED_*) */
#define DWC2_SPEED_LOW  0
#define DWC2_SPEED_FULL 1
#define DWC2_SPEED_HIGH 2

/* transaction ended in NAK/NYET/frame-overrun: nothing moved, the
   toggle is untouched and the caller may retry later */
#define DWC2_XFER_RETRY (-2)

/* power on the core (mailbox), grab the DMA pool and bring the host up.
   Returns 0 on success; on failure the driver stays inert and every
   call below is a safe no-op */
int  dwc2_init(ewokos_addr_t mmio_base);
/* full host re-init: clears wedged port-enable state machines that only
   a core reset recovers */
int  dwc2_reinit(void);
bool dwc2_ready(void);

/* root port */
bool dwc2_port_connected(void);
void dwc2_ack_port_change(void);
/* reset+enable; returns DWC2_SPEED_* or < 0 */
int  dwc2_reset_port(void);
/* latch/unlatch FS/LS-only mode: with it set the host suppresses the HS
   chirp during reset so the device enumerates at full speed */
void dwc2_force_fs_only(bool enable);
/* true when the last channel transaction died with TXERR: on a freshly
   negotiated high-speed link that never moved a byte this marks a dead
   480Mbps data path */
bool dwc2_last_xfer_txerr(void);

/* control transfer on EP0: setup/data/status with 3 attempts per stage.
   Returns bytes moved in the data stage or < 0 */
int  dwc2_control_xfer(uint8_t addr, bool low_speed, uint8_t ep_mps,
        const usb_setup_pkt_t* setup, void* data, bool data_in);

/* one interrupt-IN transaction on the dedicated HID channel. The toggle
   is resynced from the core's next-PID view on every outcome.
   Returns >0 data bytes, 0 no data (NAK/DTERR swallowed),
   DWC2_XFER_RETRY on endpoint STALL, -1 hard error */
int  dwc2_int_in_xfer(uint8_t addr, bool low_speed, uint8_t ep_num,
        uint16_t max_packet, uint8_t* toggle, void* data, uint16_t size);

/* one bulk transaction on the dedicated MSC channels.
   Returns bytes moved, DWC2_XFER_RETRY on NAK, -1 error */
int  dwc2_bulk_xfer(bool dir_in, uint8_t addr, bool low_speed, uint8_t ep_num,
        uint16_t max_packet, uint8_t* toggle, void* data, uint32_t length,
        uint32_t timeout_ms);

/* bulk-only transport recovery: class reset plus clearing both endpoint
   halts, then the toggles must restart at DATA0 */
void dwc2_msc_recover(uint8_t addr, bool low_speed, uint8_t ctrl_mps,
        uint8_t iface_num, uint8_t ep_in, uint8_t ep_out);

#endif /* __ARCH_BCM283X_DWC2_H */
