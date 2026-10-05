@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displaymand              
@/bin/ipcserv /drivers/waveshare/epaper4.26d     /dev/disp0 
@/bin/ipcserv /drivers/fontd                 

@/bin/ipcserv /sbin/splashd -w 800 -h 480 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

#@/bin/splash -m "start /dev/console0" -p 10
#@/bin/ipcserv /drivers/consoled       
#@set_stdio /dev/console0

@/bin/splash -m "start /dev/timer" -p 20
@/bin/ipcserv /drivers/timerd             

@/bin/splash -m "start /dev/pipe0" -p 30
#@/bin/ipcserv /drivers/nulld              /dev/null
@/bin/ipcserv /drivers/piped               /dev/pipe0

@/bin/splash -m "mount /tmp" -p 40
@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/splash -m "start /dev/wl0" -p 50
@/bin/ipcserv /drivers/raspix/wland          /dev/wl0

@/bin/splash -m "start /dev/net0" -p 60
@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0

@/bin/splash -m "start /dev/time" -p 70
@/bin/ipcserv /drivers/timed    /dev/time

@/bin/splash -m "start sessiond" -p 80
@/bin/ipcserv /sbin/sessiond

@/bin/splash -m "start sshd" -p 90
@/bin/bgrun /sbin/sshd

@/bin/splash -m "start x" -p 100
@/bin/ipcserv /drivers/xserverd           /dev/x
@/bin/bgrun /bin/x/xsession misa 
