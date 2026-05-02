@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/raspix/fbd      /dev/fb0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "start /dev/tty0" -p 10
@/bin/ipcserv /drivers/raspix/uartd         /dev/tty0


@/bin/splash -m "start /dev/timer" -p 20
@/bin/ipcserv /drivers/timerd

@/bin/splash -m "start /dev/touch0" -p 30
@/bin/ipcserv /drivers/waveshare/gt911_touchd  /dev/touch0

@/bin/splash -m "mount /tmp" -p 40
@/bin/ipcserv /drivers/ramfsd          /tmp

@/bin/splash -m "start /dev/null" -p 50
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/splash -m "start /dev/wl0" -p 60
@/bin/ipcserv /drivers/raspix/wland          /dev/wl0

@/bin/splash -m "start /dev/net0" -p 70
@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0

@/bin/splash -m "start /dev/time" -p 80
@/bin/ipcserv /drivers/timed    /dev/time

@/bin/splash -m "run sessiond" -p 90
@/bin/ipcserv /sbin/sessiond

#@/bin/splash -m "load fonts" -p 95
#@/bin/load_font

@/bin/splash -m "start x" -p 100
@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/bgrun /sbin/x/xtouch
@/bin/bgrun /sbin/x/xim_vkey -h 168
@/bin/bgrun /bin/x/xsession  misa
