# Q8B boot RAMFS profile.  /sbin/init runs this before selecting the
# persistent rootfs; the normal profile below runs after that switch.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
# Serial rescue console: survives switchroot (the tty0 mount moves to the
# new root's /dev), so a stalled persistent profile can still be probed
# interactively over the 40-pin UART (login root/root).
@/bin/ipcserv /drivers/8gen3/uartd        /dev/tty0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
