#@/bin/ipcserv /drivers/raspix/uartd       /dev/tty0
#@set_stdio /dev/tty0

@/bin/ipcserv /drivers/displayd           
@/bin/ipcserv /drivers/waveshare/lcdhatd  /dev/fb0 
@/bin/ipcserv /drivers/fontd              

@/bin/ipcserv /sbin/splashd -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "loading fonts" -p 5
@/bin/load_font

@/bin/splash -m "loading /dev/console0" -p 10
@export UX_ID=0
@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0
@export KLOG_DEV=/dev/console0

@/bin/splash -m "loading timer" -p 40
@/bin/ipcserv /drivers/timerd             

#@/bin/ipcserv /drivers/nulld              /dev/null
#@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/splash -m "loading touch" -p 65
@/bin/bgrun /sbin/x/xtouch  /dev/fb0

@/bin/splash -m "loading xim_vkey" -p 70
@/bin/bgrun /sbin/x/xim_vkey 

@/bin/splash -m "start sessiond" -p 80
@/bin/ipcserv /sbin/sessiond

@/bin/splash -m "start xserver" -p 90
@/bin/ipcserv /drivers/xserverd           /dev/x

@/bin/splash -m "startx..." -p 100
@/bin/bgrun /bin/x/xsession misa 
