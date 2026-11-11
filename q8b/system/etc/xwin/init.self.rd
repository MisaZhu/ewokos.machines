# Q8B self-root (initramfs-only) profile: headless wlan/net test system.
#
# Used when the boot RAMFS stays root (kernel.conf rootfs=self): everything
# runs from the PXE-loaded initramfs, the SD card is never mounted.  The boot
# RAMFS profile already owns uartd, sessiond and the /dev/tty0 session, and
# pcied is started before the boot profile runs, so none of those appear here.
# Display/USB-input/desktop stacks are intentionally omitted.
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
@/bin/ipcserv /drivers/logd              /dev/log
# QCNFA765 station: scans on its own and joins the highest-priority network
# in /etc/wlan/network.json.  Control it with
#   devcmd /dev/wl0 help|log|state|scan|list|connect <ssid> <passwd>
@/bin/ipcserv /drivers/q8b/wlan          /dev/wl0 --start
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/wl0
@/bin/bgrun /sbin/sshd
