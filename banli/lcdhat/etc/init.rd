#@/bin/ipcserv /drivers/raspix/uartd       /dev/tty0
#@set_stdio /dev/tty0

@/bin/ipcserv /drivers/displayd           
@/bin/ipcserv /drivers/banli/lcdhatd  /dev/fb0 
@/bin/ipcserv /drivers/fontd              
@/bin/load_font

@export UX_ID=0
@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0
@export KLOG_DEV=/dev/console0

@/bin/ipcserv /drivers/banli/gamekbd  /dev/keyb0
@/bin/ipcserv /drivers/vkeybd             /dev/vkeyb /dev/keyb0 -t j
@/bin/ipcserv /drivers/vjoystickd         /dev/vjoystick /dev/vkeyb -m

@/bin/ipcserv /drivers/timerd             
#@/bin/ipcserv /drivers/nulld              /dev/null
#@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/ipcserv /sbin/sessiond

@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick 
@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick
@/bin/bgrun /sbin/x/xim_vkey 

@/bin/ipcserv /drivers/xserverd           /dev/x
@/bin/bgrun /bin/x/xsession misa 
