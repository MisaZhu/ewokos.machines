@/bin/ipcserv /drivers/logd  /dev/log

@/bin/ipcserv /drivers/displaymand         
@/bin/ipcserv /drivers/versatilepb/fbdis/dev/disp0dev/fb0
@/bin/ipcserv /drivers/fontd -l -o

@export UX_ID=0
@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 14
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."