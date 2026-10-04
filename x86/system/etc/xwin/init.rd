@/bin/ipcserv /drivers/x86/ttyd /dev/tty0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/x86/fbdisplayd /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "start /dev/timer" -p 30
@/bin/ipcserv /drivers/timerd

@/bin/splash -m "start /dev/null" -p 40
@/bin/ipcserv /drivers/nulld /dev/null
@/bin/ipcserv /drivers/piped  /dev/pipe0
@/bin/ipcserv /drivers/ramfsd /tmp

#@/bin/ipcserv /drivers/x86/ps2keybd /dev/keyb0
#@/bin/ipcserv /drivers/x86/ps2moused /dev/mouse0

@/bin/splash -m "start /dev/hid0" -p 50
@/bin/ipcserv /drivers/x86/usbhostd    /dev/hid0

@/bin/splash -m "start /dev/keyb0" -p 60
@/bin/ipcserv /drivers/x86/hid_keybd   /dev/keyb0  /dev/hid0

@/bin/splash -m "start /dev/mouse0" -p 65
@/bin/ipcserv /drivers/x86/hid_moused  /dev/mouse0 /dev/hid0

@/bin/splash -m "load fonts" -p 80
@/bin/load_font

@/bin/splash -m "start x" -p 88
@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/splash -m "start xmouse" -p 92
@/bin/bgrun /sbin/x/xmouse

@/bin/splash -m "start xim" -p 96
@/bin/bgrun /sbin/x/xim_none

@/bin/splash -m "start session" -p 100
@/bin/bgrun /bin/x/xsession  misa
