#ifndef UC_POWER_H
#define UC_POWER_H

#include <stdint.h>

/*
 * VC firmware power domains (RPI_FIRMWARE_SET_DOMAIN_STATE interface).
 * The firmware domain index is the DT binding index + 1
 * (see raspberrypi-power.c: dom->domain = xlate_index + 1).
 * DT indices from dt-bindings/power/raspberrypi-power.h.
 */
#define UC_PWR_DOMAIN_VIDEO_SCALER  (3 + 1)   /* HVS */
#define UC_PWR_DOMAIN_DSI1          (18 + 1)  /* DSI1 controller + analog PHY */

/*
 * Ask the VC firmware (property mailbox) to power a domain on/off.
 * NOTE: on the real uConsole image the &dsi1 node carries NO
 * power-domains property in any bcm27xx dtsi — Linux never touches
 * the firmware power domain for DSI1, i.e. the firmware boots with it
 * usable.  These helpers exist only to VERIFY that state and to
 * enable the domain in case a firmware build boots with it off.
 * Never power-cycle it: that is a path real Linux never exercises.
 * Returns 0 on firmware ACK, -1 otherwise.
 */
int uc_power_domain_set(uint32_t domain, int on);

/* Query a domain: 1 = on, 0 = off, -1 = mailbox failure. */
int uc_power_domain_get(uint32_t domain);

#endif
