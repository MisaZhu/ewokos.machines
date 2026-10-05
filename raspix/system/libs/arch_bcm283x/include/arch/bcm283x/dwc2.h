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

/* HPRT bit layout, exposed so a caller can decode dwc2_port_status() in a
   log line without duplicating the register map */
#define DWC2_HPRT_CONNSTAT      (1u << 0)
#define DWC2_HPRT_ENA           (1u << 2)
#define DWC2_HPRT_OVRCURRACT    (1u << 4)
#define DWC2_HPRT_PWR           (1u << 12)
#define DWC2_HPRT_LINESTS_SHIFT 10   /* 2 bits: D+/D- line state  */
#define DWC2_HPRT_SPEED_SHIFT   17   /* 2 bits: 0=high 1=full 2=low */

/* HCINT bit layout, exposed for the same reason: "TXERR" alone collapses five
   different failures into one word. ACK/STALL/NAK all prove the device is on
   the bus and answering, AHBERR points at the DMA buffer rather than the wire,
   and TXERR on its own is the only one that really means "nothing came back". */
#define DWC2_HCINT_XFRC         (1u << 0)
#define DWC2_HCINT_CHH          (1u << 1)
#define DWC2_HCINT_AHBERR       (1u << 2)
#define DWC2_HCINT_STALL        (1u << 3)
#define DWC2_HCINT_NAK          (1u << 4)
#define DWC2_HCINT_ACK          (1u << 5)
#define DWC2_HCINT_NYET         (1u << 6)
#define DWC2_HCINT_TXERR        (1u << 7)
#define DWC2_HCINT_BBLERR       (1u << 8)
#define DWC2_HCINT_FRMOVRUN     (1u << 9)
#define DWC2_HCINT_DTERR        (1u << 10)

/* which stage of the last control transfer was in flight when it failed.
   SET_ADDRESS is two transactions, not one -- a SETUP the device may well
   accept, then a zero-length IN status stage it may not -- and telling those
   apart is the difference between "the device never answered" and "the device
   answered and then went away". */
#define DWC2_XFER_STAGE_NONE    0
#define DWC2_XFER_STAGE_SETUP   1
#define DWC2_XFER_STAGE_DATA    2
#define DWC2_XFER_STAGE_STATUS  3

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
/* raw HPRT for diagnostics. On a board with no console this is the only way
   to tell a device that refuses to talk from a port the host had to give up
   on: over-current active, port enable dropped, and the D+/D- line state all
   live here and none of them reach the caller of a failed transfer */
uint32_t dwc2_port_status(void);
/* raw HCINT of the last failed channel transaction, and the control stage it
   died in (DWC2_XFER_STAGE_*) */
uint32_t dwc2_last_xfer_hcint(void);
uint32_t dwc2_last_xfer_stage(void);

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
