@/bin/ipcserv /drivers/displayd       
@/bin/ipcserv /drivers/miyoo/fbd      /dev/fb0
@/bin/ipcserv /drivers/fontd          

@export UX_ID=0
@/bin/ipcserv /sbin/splashd -w 480 -h 320
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "loading console klog" -p 5
@export UX_ID=2
@/bin/ipcserv /drivers/consoled  /dev/klog
@export KLOG_DEV=/dev/klog
@set_stdio /dev/klog

#@/bin/ipcserv /drivers/miyoo/ms_uartd /dev/tty0

@/bin/splash -m "loading joystick" -p 10
@/bin/ipcserv /drivers/miyoo/gpio_joystickd     /dev/joystick
@/bin/ipcserv /drivers/vkeybd                   /dev/vkeyb /dev/joystick -t j
@/bin/ipcserv /drivers/vjoystickd               /dev/vjoystick /dev/vkeyb -m
#@/bin/ipcserv /drivers/miyoo/audctrl            /dev/sound

@/bin/splash -m "loading timer" -p 20
@/bin/ipcserv /drivers/timerd         

@/bin/splash -m "loading null" -p 30
@/bin/ipcserv /drivers/nulld          /dev/null

@/bin/splash -m "loading ramfs" -p 40
@/bin/ipcserv /drivers/ramfsd         /tmp

@/bin/splash -m "starting sessiond" -p 50
@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r -t /dev/tty0 

@/bin/splash -m "loading fonts" -p 60
@/bin/load_font

@/bin/splash -m "loading X input" -p 80
@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick 
@/bin/bgrun /sbin/x/xmouse    /dev/vjoystick 

@/bin/splash -m "starting X" -p 100
@/bin/ipcserv /drivers/xserverd       /dev/x
@/bin/bgrun /sbin/x/xim_vkey -h 120

@/bin/bgrun /bin/x/xsession misa 
