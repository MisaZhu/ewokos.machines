@/bin/ipcserv /drivers/logd /dev/log

#@/bin/ipcserv /drivers/rk3506/uartd       /dev/tty0
#@set_stdio /dev/tty0

@/bin/ipcserv /drivers/displaymand             
@/bin/ipcserv /drivers/rk3506/fbdisplayd            /dev/disp0
@/bin/ipcserv /drivers/fontd                

#@/bin/ipcserv /drivers/consoled             /dev/console0
#@set_stdio /dev/console0

@/bin/ipcserv /sbin/splashd -d
@/bin/splash -i /usr/system/images/logos/clockwork.png -m "start..."

#@/bin/splash -m "loading fonts" -p 5
#@/bin/load_font

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

@/bin/splash -m "start  xim" -p 85
@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick
#@/bin/bgrun /sbin/x/xim_vkey -w 560 -h 168

@/bin/splash -m "start  xmouse" -p 90
@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick

@/bin/splash -m "starting X" -p 100
@/bin/ipcserv /drivers/xserverd             /dev/x

@/bin/bgrun /bin/x/xsession misa
