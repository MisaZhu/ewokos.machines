@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log


@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/raspi5/fbd      /dev/fb0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "start /dev/tty0" -p 10
@/bin/ipcserv /drivers/raspi5/uartd         /dev/tty0


@/bin/splash -m "run sessiond" -p 12
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/splash -m "start /dev/timer" -p 20
@/bin/ipcserv /drivers/timerd

@/bin/splash -m "start /dev/hid0" -p 25
@/bin/ipcserv /drivers/raspi5/usbhostd    /dev/hid0

@/bin/splash -m "start /dev/keyb0" -p 28
@/bin/ipcserv /drivers/raspi5/hid_keybd   /dev/keyb0  /dev/hid0
@/bin/ipcserv /drivers/raspi5/hid_moused  /dev/mouse0 /dev/hid0

@/bin/splash -m "start /dev/touch0" -p 30
@/bin/ipcserv /drivers/raspi5/gt911_touchd /dev/touch0

@/bin/splash -m "mount /tmp" -p 40
@/bin/ipcserv /drivers/ramfsd          /tmp

@/bin/splash -m "start /dev/null" -p 50
@/bin/ipcserv /drivers/nulld           /dev/null

# No sound/wifi/bluetooth driver exists under drivers/raspi5 yet, and netd
# needs one of them for its backing net device. ipcserv waits forever with
# ipc_ping when the server it spawned never registers, so leaving these
# enabled wedges init here instead of reaching xserverd.
@/bin/splash -m "start /dev/sound0" -p 55
#@/bin/ipcserv /drivers/raspi5/soundd           /dev/sound0

@/bin/splash -m "start /dev/wl0" -p 60
#@/bin/ipcserv /drivers/raspi5/wland          /dev/wl0

@/bin/splash -m "start /dev/net0" -p 70
#@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0

@/bin/splash -m "start /dev/time" -p 80
@/bin/ipcserv /drivers/timed    /dev/time

#@/bin/splash -m "start telnetd" -p 83
#@/bin/bgrun /sbin/telnetd

#@/bin/splash -m "start sshd" -p 84
#@/bin/bgrun /sbin/sshd

#@/bin/splash -m "start /dev/bt0" -p 85
#@/bin/ipcserv /drivers/raspi5/btd    /dev/bt0

@/bin/splash -m "start x" -p 100
@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/bgrun /sbin/x/xtouch
# xim_vkey is the on-screen keyboard only; it never opens /dev/keyb0, and
# nothing else forwards /dev/keyb0 or /dev/mouse0 into xserverd. Without these
# two a physical USB keyboard/mouse enumerates but produces no X input at all.
@/bin/bgrun /sbin/x/xmouse
@/bin/bgrun /sbin/x/xim_none
@/bin/bgrun /sbin/x/xim_vkey -h 168
@/bin/bgrun /bin/x/xsession  misa
