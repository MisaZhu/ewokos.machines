# Q8B full window system on the UEFI GOP framebuffer.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/nulld             /dev/null
@/bin/ipcserv /drivers/logd              /dev/log

# Serial logging is available during GPU initialization. Wait for g2dd
# before starting display/Graph clients so their first canvas can use it.
@/bin/ipcserv /drivers/q8b/uartd         /dev/tty0
@export ADRENO_ENABLE=1
@/bin/ipcserv /drivers/q8b/g2dd          /dev/g2d

# PCIe3a/TC9563 Ethernet and DHCP network stack.
@/bin/ipcserv /drivers/q8b/net           /dev/eth0
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/eth0
@/bin/bgrun /sbin/sshd
@/bin/ipcserv /drivers/q8b/fand          /dev/fan

@/bin/ipcserv /drivers/q8b/usbhostd      /dev/hid0
@/bin/ipcserv /drivers/q8b/hid_keybd     /dev/keyb0  /dev/hid0
@/bin/ipcserv /drivers/q8b/hid_moused    /dev/mouse0 /dev/hid0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/q8b/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /drivers/xserverd          /dev/x
@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /bin/x/xsession misa
