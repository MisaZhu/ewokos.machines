@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displaymand       
@/bin/ipcserv /drivers/miyoo/fbdisplayd      /dev/disp0
@/bin/ipcserv /drivers/fontd          

@/bin/ipcserv /sbin/splashd -w 480 -h 320
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

#@/bin/ipcserv /drivers/consoled     /dev/console0 
#@set_stdio /dev/console0

#@/bin/splash -m "start /dev/tty0" -p 5
#@/bin/ipcserv /drivers/miyoo/ms_uartd /dev/tty0

@/bin/splash -m "loading joystick" -p 10
@/bin/ipcserv /drivers/miyoo/gpio_joystickd     /dev/joystick


@/bin/splash -m "start vkeyb" -p 12
@/bin/ipcserv /drivers/vkeybd              -t j  /dev/vkeyb /dev/joystick

@/bin/splash -m "start vjoystick" -p 15
@/bin/ipcserv /drivers/vjoystickd          -m   /dev/vjoystick /dev/vkeyb 

@/bin/splash -m "loading timer" -p 20
@/bin/ipcserv /drivers/timerd         

@/bin/splash -m "start /dev/sound0" -p 25
@/bin/ipcserv /drivers/miyoo/audctrl             /dev/sound0

@/bin/splash -m "loading null" -p 30
@/bin/ipcserv /drivers/nulld          /dev/null

@/bin/splash -m "loading ramfs" -p 40
@/bin/ipcserv /drivers/ramfsd         /tmp

@/bin/splash -m "starting sessiond" -p 50
@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r -t /dev/tty0 

#@/bin/splash -m "loading fonts" -p 60
#@/bin/load_font

@/bin/splash -m "loading X input" -p 80
@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick 
@/bin/bgrun /sbin/x/xmouse    /dev/vjoystick 
@/bin/bgrun /sbin/x/xim_vkey -h 120

#@/bin/ipcserv /drivers/miyoo/g2dd     /dev/g2d

@/bin/splash -m "starting X" -p 100
@/bin/ipcserv /drivers/xserverd       /dev/x

@/bin/bgrun /bin/x/xsession misa 
