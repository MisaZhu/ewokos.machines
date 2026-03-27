@/bin/ipcserv /drivers/logd /dev/log

#@/bin/ipcserv /drivers/raspix/uartd       /dev/tty0
#@set_stdio /dev/tty0

@/bin/ipcserv /drivers/displayd           
@/bin/ipcserv /drivers/gnpe/lcdhatd  /dev/fb0 
@/bin/ipcserv /drivers/fontd              

@export UX_ID=0
@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0

#@/bin/ipcserv /drivers/gnpe/gamekbd  /dev/keyb0
#@/bin/ipcserv /drivers/vkeybd             /dev/vkeyb /dev/keyb0 -t j
#@/bin/ipcserv /drivers/vjoystickd         /dev/vjoystick /dev/vkeyb -m

@/bin/ipcserv /drivers/timerd             
#@/bin/ipcserv /drivers/nulld              /dev/null
#@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/ipcserv /sbin/sessiond

#@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick 
@/bin/bgrun /sbin/x/xtouch     /dev/fb0
@/bin/bgrun /sbin/x/xim_vkey 

@/bin/load_font
@/bin/ipcserv /drivers/xserverd           /dev/x
@/bin/bgrun /bin/x/xsession misa 
