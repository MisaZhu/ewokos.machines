@/bin/ipcserv /drivers/logd /dev/log 

@/bin/ipcserv /drivers/displayd       
@/bin/ipcserv /drivers/xgo/spilcdd    /dev/fb0
@/bin/ipcserv /drivers/fontd          

@export UX_ID=0
@/bin/ipcserv /drivers/consoled  /dev/console0 
set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd         

@/bin/ipcserv /drivers/xgo/xgo_buttond    /dev/keyb0
@/bin/ipcserv /drivers/xgo/soundd    /dev/sound0
@/bin/ipcserv /drivers/raspix/uartd   /dev/tty0

@/bin/ipcserv /drivers/xserverd       /dev/x

@/bin/bgrun /sbin/x/xim_none   /dev/keyb0 
@/bin/bgrun /sbin/x/xim_vkey -h 120

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/x/xsession misa

#@/bin/bgrun /apps/xgo/xgo 

