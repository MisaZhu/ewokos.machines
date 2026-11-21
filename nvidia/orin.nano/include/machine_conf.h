#ifndef ORIN_MACHINE_CONF_H
#define ORIN_MACHINE_CONF_H

/* Consumed by the shared /sbin/init (system/basic/sys/init/init.c): the
 * rescue console and early driver paths live under /drivers/orin.nano/. */
#define INIT_DRIVER_DIR "orin.nano"

/* The shared init's early PCIe-owner-before-USB startup is a Q8B WCN6855
 * workaround.  Orin's C1 (M.2 Key-E) cold-own is still disabled at boot (it
 * raises an EL3 RAS), so do not force pcied ahead of the boot profile. */
#define INIT_EARLY_PCIE_OWNER 0

/* SYS_PLATFORM_CTL op: PSCI SYSTEM_RESET via TF-A BL31 (whole-SoC reset).
 * Used by /bin/reboot and the automation pipeline to reset the board. */
#define ORIN_PLATFORM_RESET 1u

#endif