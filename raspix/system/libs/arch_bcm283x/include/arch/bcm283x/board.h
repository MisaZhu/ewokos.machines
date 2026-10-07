#ifndef ARCH_BCM283X_BOARD_H
#define ARCH_BCM283X_BOARD_H

#include <stdint.h>

/* Which Broadcom bluetooth combo chip is soldered to a BCM283x Raspberry Pi.
   The HCI patchram is chip-specific, so a transport must pick it by chip
   rather than by the coarse sysinfo.machine string: "raspberry-pi3b" is
   shared by the Pi 3B (CYW43430A1) and the Pi 3B+ (CYW4345C0), which need
   different firmware. The raw board revision (mailbox GET_BOARD_REVISION)
   is the ground truth that separates them. */
typedef enum {
    BCM283X_BT_NONE  = 0,  /* no BCM283x bluetooth radio (Pi 0/1/2B, CM3, Pi5) */
    BCM283X_BT_43430 = 1,  /* CYW43430A1: Pi 3B, Zero W, Zero 2 W */
    BCM283X_BT_43455 = 2   /* CYW4345C0: Pi 3A+, 3B+, 4B, 400, CM4 */
} bcm283x_bt_chip_t;

/* raw GET_BOARD_REVISION mailbox value; 0 when the mailbox is unavailable.
   Maps the MMIO window on first use. */
uint32_t bcm283x_board_revision(void);

/* classified bluetooth chip for this board, cached after the first call. */
bcm283x_bt_chip_t bcm283x_bt_chip(void);

#endif
