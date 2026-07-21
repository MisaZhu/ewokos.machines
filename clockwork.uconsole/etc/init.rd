@export TZ=CST-8
@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/clockwork/powerd      /dev/power0
@/bin/ipcserv /drivers/displayd              
@/bin/ipcserv /drivers/clockwork/fbd         /dev/fb0
@/bin/ipcserv /drivers/fontd                 

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

#@/bin/ipcserv /drivers/consoled              /dev/console0
#@set_stdio /dev/console0

#@/bin/load_font

#@/bin/splash -m "start /dev/tty0" -p 10
#@/bin/ipcserv /drivers/raspix/uartd          /dev/tty0

@/bin/splash -m "start /dev/timer" -p 10
@/bin/ipcserv /drivers/timerd                

@/bin/splash -m "start /dev/usbhostd" -p 20
@/bin/ipcserv /drivers/raspix/usbhostd       /dev/hid0

@/bin/splash -m "start /dev/usb_keyb" -p 30
@/bin/ipcserv /drivers/raspix/hid_keybd      /dev/keyb0

@/bin/splash -m "start /dev/usb_mouse" -p 40
@/bin/ipcserv /drivers/raspix/hid_moused     /dev/mouse0

#@/bin/splash -m "start /dev/usb_joystick" -p 45
#@/bin/ipcserv /drivers/raspix/hid_joystickd  /dev/joystick0

@/bin/splash -m "start /dev/vkeyb" -p 45
@/bin/ipcserv /drivers/vkeybd                /dev/vkeyb /dev/keyb0

@/bin/splash -m "start /dev/null" -p 50
@/bin/ipcserv /drivers/nulld                 /dev/null

@/bin/splash -m "mount /tmp" -p 60
@/bin/ipcserv /drivers/ramfsd                /tmp

@/bin/splash -m "start /dev/wl0" -p 70
@/bin/ipcserv /drivers/raspix/wland          /dev/wl0

@/bin/splash -m "start /dev/net0" -p 80
@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0

@/bin/splash -m "start /dev/time" -p 85
@/bin/ipcserv /drivers/timed

@/bin/splash -m "start sessiond" -p 90
@/bin/ipcserv /sbin/sessiond

#@/bin/splash -m "start telnetd" -p 90
#@/bin/bgrun /sbin/telnetd

@/bin/splash -m "start sshd" -p 95
@/bin/bgrun /sbin/sshd


@/bin/splash -m "start xmouse" -p 95
@/bin/bgrun /sbin/x/xmouse /dev/mouse0 

@/bin/splash -m "start xim" -p 96
@/bin/bgrun /sbin/x/xim_none /dev/vkeyb

#@/bin/splash -m "start xim" -p 96
#@/bin/bgrun /sbin/x/xim_none /dev/joystick0

#@/bin/splash -m "start /dev/sound0" -p 96
#@/bin/ipcserv /drivers/raspix/soundd           /dev/sound0

@/bin/splash -m "start x" -p 100
@/bin/ipcserv /drivers/xserverd              /dev/x

@/bin/bgrun /bin/x/xsession misa 
