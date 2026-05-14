@/bin/bgrun /bin/ipcserv /drivers/x86/ttyd /dev/tty0

@/bin/bgrun /bin/ipcserv /drivers/timerd
@/bin/bgrun /bin/ipcserv /drivers/ramfsd /tmp
@/bin/bgrun /bin/ipcserv /drivers/nulld /dev/null
@/bin/bgrun /bin/ipcserv /sbin/sessiond

@/bin/bgrun /bin/session -r -t /dev/tty0
