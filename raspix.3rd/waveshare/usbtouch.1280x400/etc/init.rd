#@/bin/ipcserv /drivers/raspix/spi2uartd     /dev/tty0 nr
@/bin/ipcserv /drivers/raspix/uartd     /dev/tty0 nr
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0 

@/bin/ipcserv /drivers/raspix/fbd           /dev/fb0
@/bin/ipcserv /drivers/displayd             
@/bin/ipcserv /drivers/fontd                

@export UX_ID=0
@/bin/ipcserv /drivers/consoled             
set_stdio /dev/console0
@export KLOG_DEV=/dev/console0

@/bin/ipcserv /drivers/raspix/usbd          /dev/touch0
@/bin/ipcserv /drivers/timerd               
@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/ipcserv /drivers/ramfsd               /tmp


@/bin/ipcserv /drivers/xserverd             /dev/x
@/bin/bgrun /sbin/x/xtouch 
@/bin/bgrun /sbin/x/xim_vkey 

@/bin/bgrun /bin/x/xsession misa 