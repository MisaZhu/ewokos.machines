@/bin/ipcserv /drivers/rk3506/uartd       /dev/tty0

@export KLOG_DEV=/dev/tty0
@set_stdio /dev/tty0
@/bin/bgrun /bin/shell

@/bin/ipcserv /drivers/rk3506/fbd            /dev/fb0
@/bin/ipcserv /drivers/displayd             
@/bin/ipcserv /drivers/fontd                
@/bin/load_font

@export UX_ID=0
@/bin/ipcserv /drivers/consoled             /dev/klog
@export KLOG_DEV=/dev/klog
@set_stdio /dev/klog

@/bin/ipcserv /drivers/rk3506/kbd  /dev/keyb0
@/bin/ipcserv /drivers/vkeybd      /dev/vkeyb /dev/keyb0
@/bin/ipcserv /drivers/vjoystickd  /dev/vjoystick /dev/vkeyb -s 133 -m

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/ipcserv /drivers/ramfsd               /tmp

@/bin/ipcserv /sbin/sessiond

@export UX_ID=1
@/bin/ipcserv /drivers/consoled   /dev/console1 -i /dev/keyb0
@/bin/bgrun /bin/session -r -t /dev/console1

@/bin/ipcserv /drivers/xserverd             /dev/x
@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick
@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick
#@/bin/bgrun /sbin/x/xim_vkey 560 168
@/bin/bgrun /bin/x/xsession misa
