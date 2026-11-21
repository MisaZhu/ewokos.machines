# Orin Nano boot RAMFS profile (packed as /etc/init.ramfs.rd).
#
# No console daemon is started here on purpose: the image ships no
# /etc/init.rd and kernel.conf selects rootfs=self, so init's self-root path
# falls through to its serial rescue console right after this profile
# returns.  uartd/sessiond/session therefore start exactly once (root/root
# login on the J14 debug UART, 115200 8N1).
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
# PCIe C1 (M.2 Key-E) host owner: cold-owns the root complex via BPMP.
# DISABLED for bring-up: BPMP HSP doorbell access (0x03c50300) triggers a RAS
# bus error / core power-off on this board.  Re-enable once BPMP is debugged.
# @/bin/ipcserv /drivers/orin.nano/pcied   /dev/pcie0
# I2C masters / GPIO: DISABLED for bring-up.  These peripherals need BPMP
# clock enable; BPMP access itself faults (RAS), so the unclocked I2C/GPIO
# register access raises an SError -> EL3 crash.  Re-enable once BPMP works.
# @/bin/ipcserv /drivers/orin.nano/i2cd    /dev/i2c0
# @/bin/ipcserv /drivers/orin.nano/i2cd    /dev/i2c1
# @/bin/ipcserv /drivers/orin.nano/gpiod   /dev/gpio0
# Ethernet (RTL8168 on PCIe C8): cold-own via BPMP, run at boot as the bring-up
# test.  Progress/crash markers land on the serial console; recover with the
# Pico GPIO hardware reset.  MAC confirmed up, so netd (DHCP) now starts too.
@/bin/ipcserv /drivers/orin.nano/net     /dev/eth0
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/eth0