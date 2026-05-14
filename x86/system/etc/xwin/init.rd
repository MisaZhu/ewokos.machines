@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/x86/fbd /dev/fb0
@/bin/ipcserv /drivers/fontd
@/bin/ipcserv /drivers/x86/ttyd /dev/tty0
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/nulld /dev/null
@/bin/ipcserv /drivers/ramfsd /tmp
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
