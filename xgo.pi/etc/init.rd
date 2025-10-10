@/bin/ipcserv /drivers/logd /dev/log 

@/bin/ipcserv /drivers/displayd       
@/bin/ipcserv /drivers/xgo/spilcdd    /dev/fb0
@/bin/ipcserv /drivers/fontd          

@export UX_ID=0
@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 14

@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@export UX_ID=1
@/bin/ipcserv /drivers/consoled  /dev/console0 
@set_stdio /dev/console0

@/bin/splash -m "start timer" -p 10
@/bin/ipcserv /drivers/timerd         

@/bin/splash -m "driver buttond" -p 20
@/bin/ipcserv /drivers/xgo/xgo_buttond    /dev/keyb0
@/bin/ipcserv /drivers/vkeybd             /dev/vkeyb /dev/keyb0 -t k

@/bin/splash -m "driver soundd" -p 30
@/bin/ipcserv /drivers/xgo/soundd    /dev/sound0

@/bin/splash -m "driver uart" -p 35
@/bin/ipcserv /drivers/raspix/uartd   /dev/tty0

@/bin/splash -m "start xserver" -p 40
@/bin/ipcserv /drivers/xserverd       /dev/x

@/bin/splash -m "start xim_none" -p 50
@/bin/bgrun /sbin/x/xim_none   /dev/vkeyb

@/bin/splash -m "start xim_vkey" -p 60
@/bin/bgrun /sbin/x/xim_vkey -h 120

@/bin/splash -m "start sessiond" -p 70
@/bin/ipcserv /sbin/sessiond

@/bin/splash -m "start x" -p 100
@/bin/bgrun /bin/x/xsession misa

#@/bin/bgrun /apps/xgo/xgo 

