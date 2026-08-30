@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log

#@/bin/ipcserv /drivers/raspix/uartd       /dev/tty0
#@set_stdio /dev/tty0

@/bin/ipcserv /drivers/displaymand           
@/bin/ipcserv /drivers/gnpe/lcdhatd  /dev/disp0 
@/bin/ipcserv /drivers/fontd              

@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd             
#@/bin/ipcserv /drivers/nulld              /dev/null
@/bin/ipcserv /drivers/piped              /dev/pipe0
@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/ipcserv /sbin/sessiond

@/bin/bgrun /sbin/x/xtouch     /dev/disp0
@/bin/bgrun /sbin/x/xim_vkey 

@/bin/load_font
@/bin/ipcserv /drivers/xserverd           /dev/x
@/bin/bgrun /bin/x/xsession misa 
