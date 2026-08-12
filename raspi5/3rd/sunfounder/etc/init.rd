@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/sunfounder/i2cfbd /dev/fb0
@/bin/ipcserv /drivers/fontd

@export UX_ID=0
@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd /tmp
@/bin/ipcserv /drivers/nulld /dev/null

@/bin/ipcserv /sbin/sessiond
