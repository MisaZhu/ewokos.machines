@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log


@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/raspix/dsi_fbdisplayd  /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

#@/bin/splash -m "start /dev/tty0" -p 10
#@/bin/ipcserv /drivers/raspix/uartd         /dev/tty0


@/bin/splash -m "run sessiond" -p 12
@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/splash -m "start /dev/timer" -p 20
@/bin/ipcserv /drivers/timerd

#@/bin/ipcserv /drivers/waveshare/gt911_touchd  /dev/touch0
@/bin/ipcserv /drivers/waveshare/dsi_touchd  /dev/touch0

@/bin/splash -m "start /dev/hid0" -p 25
@/bin/ipcserv /drivers/raspix/usbhostd    /dev/hid0

@/bin/splash -m "start /dev/keyb0" -p 28
@/bin/ipcserv /drivers/raspix/hid_keybd   /dev/keyb0  /dev/hid0
@/bin/ipcserv /drivers/raspix/hid_moused  /dev/mouse0 /dev/hid0

@/bin/splash -m "mount /tmp" -p 40
@/bin/ipcserv /drivers/piped           /dev/pipe0
@/bin/ipcserv /drivers/ramfsd          /tmp

@/bin/splash -m "start /dev/null" -p 50
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/splash -m "start /dev/wl0" -p 60
@/bin/ipcserv /drivers/raspix/wland          /dev/wl0

@/bin/splash -m "start /dev/net0" -p 70
@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0

@/bin/splash -m "start /dev/time" -p 80
@/bin/ipcserv /drivers/timed    /dev/time

#@/bin/splash -m "start telnetd" -p 83
#@/bin/bgrun /sbin/telnetd

@/bin/splash -m "start sshd" -p 84
@/bin/bgrun /sbin/sshd

#@/bin/splash -m "start /dev/bt0" -p 85
#@/bin/ipcserv /drivers/raspix/btd    /dev/bt0


@/bin/splash -m "start xtouch" -p 90
@/bin/bgrun /sbin/x/xtouch

#@/bin/splash -m "start xmouse" -p 93
#@/bin/bgrun /sbin/x/xmouse

@/bin/splash -m "start xim" -p 95
#@/bin/bgrun /sbin/x/xim_none
@/bin/bgrun /sbin/x/xim_vkey -h 168

@/bin/splash -m "start x" -p 100
@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/bgrun /bin/x/xsession  misa
