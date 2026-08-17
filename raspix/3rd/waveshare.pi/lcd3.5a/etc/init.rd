@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/raspix/uartd         /dev/tty0

@/bin/ipcserv /drivers/displaymand             
@/bin/ipcserv /drivers/waveshare/lcd3.5ad   /dev/disp0
@/bin/ipcserv /drivers/fontd                

@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd               

@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/ipcserv /drivers/ramfsd               /tmp

@/bin/bgrun /sbin/x/xtouch /dev/disp0 
@/bin/bgrun /sbin/x/xim_vkey 


@/bin/ipcserv /drivers/xserverd             /dev/x

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/x/xsession misa 