@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displayd              
@/bin/ipcserv /drivers/waveshare/epaperd     /dev/fb0 
@/bin/ipcserv /drivers/fontd                 

@export UX_ID=0
@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd             
#@/bin/ipcserv /drivers/nulld              /dev/null
#@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/ipcserv /drivers/xserverd           /dev/x

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/x/xsession misa 
