@/bin/ipcserv /drivers/rk3506/uartd       /dev/tty0

@export KLOG_DEV=/dev/tty0
@set_stdio /dev/tty0
@/bin/bgrun /bin/shell

@/bin/ipcserv /drivers/rk3506/fbd            /dev/fb0
@/bin/ipcserv /drivers/displayd             
@/bin/ipcserv /drivers/fontd                

@export UX_ID=0
@/bin/ipcserv /drivers/consoled             /dev/klog
@export KLOG_DEV=/dev/klog
@set_stdio /dev/klog

@/bin/ipcserv /drivers/rk3506/kbd  /dev/keyb0
#@/bin/ipcserv /drivers/vjoystickd             /dev/vjoystick /dev/joystick

@/bin/ipcserv /drivers/timerd               
@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/ipcserv /drivers/ramfsd               /tmp

@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r 

@/bin/bgrun /sbin/x/xim_none   /dev/keyb0
#@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick 

@/bin/load_font
@/bin/ipcserv /drivers/xserverd             /dev/x
#@/bin/bgrun /sbin/x/xim_vkey 560 168
@/bin/bgrun /bin/x/xsession misa 
