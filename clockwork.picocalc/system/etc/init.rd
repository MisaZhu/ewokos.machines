@/bin/ipcserv /drivers/rk3506/fbd            /dev/fb0
@/bin/ipcserv /drivers/displayd             
@/bin/ipcserv /drivers/fontd                

@export UX_ID=0
@/bin/ipcserv /sbin/splashd
@/bin/splash -i /usr/system/images/logos/clockwork.png -m "start..."

@/bin/splash -m "config tty_uart" -p 10
@/bin/ipcserv /drivers/rk3506/uartd       /dev/tty0
@export KLOG_DEV=/dev/tty0
@set_stdio /dev/tty0
@/bin/bgrun /bin/shell

@/bin/splash -m "config console klog" -p 20
@export UX_ID=6
@/bin/ipcserv /drivers/consoled             /dev/klog
@export KLOG_DEV=/dev/klog
@set_stdio /dev/klog

@/bin/splash -m "config  keyboard" -p 30
@/bin/ipcserv /drivers/rk3506/kbd  /dev/keyb0
@/bin/ipcserv /drivers/vkeybd      /dev/vkeyb /dev/keyb0

@/bin/splash -m "config  vjoystick" -p 40
@/bin/ipcserv /drivers/vjoystickd  /dev/vjoystick /dev/vkeyb -s 133 -m

@/bin/splash -m "config  timerd" -p 50
@/bin/ipcserv /drivers/timerd
@/bin/splash -m "config  nulld" -p 60
@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/splash -m "config  ramfs" -p 70
@/bin/ipcserv /drivers/ramfsd               /tmp

@/bin/splash -m "config  sessiond" -p 80
@/bin/ipcserv /sbin/sessiond

@/bin/splash -m "config  console1" -p 90
@export UX_ID=1
@/bin/ipcserv /drivers/consoled   /dev/console1 -i /dev/keyb0
@/bin/bgrun /bin/session -r -t /dev/console1

@/bin/splash -m "loading fonts" -p 95
@/bin/load_font

@/bin/splash -m "starting X" -p 100
@/bin/ipcserv /drivers/xserverd             /dev/x
@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick
@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick
#@/bin/bgrun /sbin/x/xim_vkey 560 168
@/bin/bgrun /bin/x/xsession misa
