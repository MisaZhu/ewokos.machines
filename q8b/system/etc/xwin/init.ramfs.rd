# Q8B boot RAMFS profile.  /sbin/init runs this before selecting the
# persistent rootfs; the normal profile below runs after that switch.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
# Serial rescue console: the xwin persistent profile keeps uartd disabled,
# so own the UART here; the session survives switchroot for devcmd and
# ssh-less debugging (2.4G wlan bring-up etc.).
@/bin/ipcserv /drivers/q8b/uartd         /dev/tty0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
