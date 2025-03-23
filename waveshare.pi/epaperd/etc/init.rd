@/bin/ipcserv /drivers/waveshare/epaperd     /dev/fb0 
@/bin/ipcserv /drivers/displayd              
@/bin/ipcserv /drivers/fontd                 

@/bin/ipcserv /drivers/consoled              -u 0
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd                
@/bin/ipcserv /drivers/nulld                 /dev/null
@/bin/ipcserv /drivers/ramfsd                /tmp

@/bin/ipcserv /sbin/sessiond

#@/bin/load_font
@/bin/ipcserv /drivers/xserverd              /dev/x
@/bin/bgrun /bin/x/xsession misa 