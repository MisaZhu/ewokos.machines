# Q8B minimal Wi-Fi test boot profile (packed as /etc/init.ramfs.rd).
#
# /sbin/init has already started /drivers/8gen3/pcied (/dev/pcie0) before this
# profile runs, so WLAN only needs the timer, a writable /tmp for RDDM crash
# dumps, the pipe/null nodes and its network stack.
#
# No console daemon is started here on purpose: the image ships no /etc/init.rd,
# so init's rootfs=self path falls through to its serial rescue console right
# after this profile returns.  uartd/sessiond/session therefore start exactly
# once (root/root login on the 40-pin UART).
#
# The controller boots asynchronously (--start); the serial console normally
# appears while MHI/firmware upload is still running.  Inspect progress with
#   /bin/devcmd /dev/wl0 log
#   /bin/devcmd /dev/wl0 state
#   /bin/devcmd /dev/net0 ip
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
@/bin/ipcserv /drivers/8gen3/wlan        /dev/wl0 --start
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/wl0
