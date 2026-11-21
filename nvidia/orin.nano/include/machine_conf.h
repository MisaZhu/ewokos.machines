#ifndef ORIN_MACHINE_CONF_H
#define ORIN_MACHINE_CONF_H

/* Consumed by the shared /sbin/init (system/basic/sys/init/init.c): the
 * rescue console and early driver paths live under /drivers/orin.nano/. */
#define INIT_DRIVER_DIR "orin.nano"

/* SYS_PLATFORM_CTL op: PSCI SYSTEM_RESET via TF-A BL31 (whole-SoC reset).
 * Used by /bin/reboot and the automation pipeline to reset the board. */
#define ORIN_PLATFORM_RESET 1u

#endif