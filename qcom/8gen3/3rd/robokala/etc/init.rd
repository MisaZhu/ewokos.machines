@export TZ=CST-8
@/bin/ipcserv /drivers/logd              /dev/log

@export ADRENO_ENABLE=1
@/bin/ipcserv /drivers/8gen3/g2dd          /dev/g2d

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/8gen3/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /sbin/splashd -w 320 -h 240 -f 12 -d
@/bin/splash -i /usr/system/images/logos/ewokos.png -m "start..."

@/bin/splash -m "load fonts" -p 10
@/bin/load_font

@/bin/splash -m "mount /tmp" -p 12
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
@/bin/ipcserv /drivers/ramfsd             /tmp

@/bin/splash -m "start /dev/tty0" -p 13
#@/bin/ipcserv /drivers/8gen3/uartd         /dev/tty0

@/bin/splash -m "start sessiond" -p 15
@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r -t /dev/tty0

# Start the PCIe host owner from the persistent root before endpoint drivers.
@/bin/splash -m "start /dev/pcie0" -p 20
@/bin/ipcserv /drivers/8gen3/pcied         /dev/pcie0

@/bin/splash -m "start adspas" -p 21
@/bin/adspas
@/bin/splash -m "start /dev/power0" -p 22
@/bin/ipcserv /drivers/robokala/powerd         /dev/power0

# PCIe3a/TC9563 Ethernet and DHCP network stack.
#@/bin/ipcserv /drivers/8gen3/net           /dev/eth0
#@/bin/ipcserv /drivers/netd              /dev/net0 /dev/eth0

@/bin/splash -m "start /dev/wl0" -p 30
@/bin/ipcserv /drivers/8gen3/wlan          /dev/wl0 --start
@/bin/splash -m "start /dev/net0" -p 40
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/wl0
@/bin/splash -m "start /dev/timed" -p 45
@/bin/ipcserv /drivers/timed
@/bin/splash -m "start sshd" -p 50
@/bin/bgrun /sbin/sshd

#@/bin/ipcserv /drivers/8gen3/fand          /dev/fan

@/bin/splash -m "start usb keyb" -p 60
@/bin/ipcserv /drivers/8gen3/hid_keybd     /dev/keyb0  /dev/usb0

@/bin/splash -m "start usb mouse" -p 61
@/bin/ipcserv /drivers/8gen3/hid_moused    /dev/mouse0 /dev/usb0

@/bin/splash -m "start usb touchpad" -p 62
@/bin/ipcserv /drivers/8gen3/hid_touchpadd /dev/touchpad0 /dev/usb0

@/bin/splash -m "start touchscreen" -p 63
@/bin/ipcserv /drivers/robokala/goodixd        /dev/touch0

@/bin/splash -m "start bluetooth" -p 75
@/bin/ipcserv /drivers/8gen3/btd           /dev/bt0

@/bin/splash -m "start bluetooth mouse" -p 76
@/bin/ipcserv /drivers/8gen3/hid_moused    /dev/mouse1 /dev/bt0 bt

@/bin/splash -m "start bluetooth keyb" -p 77
@/bin/ipcserv /drivers/8gen3/hid_keybd     /dev/keyb1  /dev/bt0 bt
@/bin/ipcserv /drivers/8gen3/hid_joystickd /dev/js1    /dev/bt0 bt

@/bin/splash -m "start sudod" -p 85
@/bin/bgrun /sbin/sudod

@/bin/bgrun /sbin/x/xtouch  /dev/touch0
@/bin/bgrun /sbin/x/xtouch -m

@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xmouse /dev/mouse1

@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /sbin/x/xim_none /dev/keyb1
@/bin/bgrun /sbin/x/xim_vkey &

@/bin/splash -m "startx" -p 100
@/bin/ipcserv /drivers/xserverd          /dev/x

@/bin/bgrun /bin/x/xsession misa
