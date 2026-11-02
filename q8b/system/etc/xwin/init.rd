@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
@/bin/ipcserv /drivers/q8b/uartd         /dev/tty0
# Start the PCIe host owner from the persistent root before endpoint drivers.
@/bin/ipcserv /drivers/q8b/pcied         /dev/pcie0
# Q8B full window system on the UEFI GOP framebuffer.
@/bin/ipcserv /drivers/logd              /dev/log

@export ADRENO_ENABLE=1
@/bin/ipcserv /drivers/q8b/g2dd          /dev/g2d

# PCIe3a/TC9563 Ethernet and DHCP network stack.
#@/bin/ipcserv /drivers/q8b/net           /dev/eth0
#@/bin/ipcserv /drivers/netd              /dev/net0 /dev/eth0

# Start QCNFA765 before its network stack.  The driver shares the raspi5
# interface: node /dev/wl0, "devcmd /dev/wl0 connect|scan|list|state" and the
# network list in /etc/wlan/network.json.  It scans and joins the highest
# priority configured network on its own; a legacy /etc/wlan.conf is still
# honoured when network.json has no entry.  The netd instance has no static
# address arguments, so it starts its DHCP client automatically on /dev/wl0.
@/bin/ipcserv /drivers/q8b/wlan          /dev/wl0 --start
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/wl0
@/bin/bgrun /sbin/sshd
#@/bin/ipcserv /drivers/q8b/fand          /dev/fan

# init starts usbhostd in the boot RAMFS so it can probe a USB rootfs before
# switching to this persistent root.  Do not start a second xHCI owner here.
@/bin/ipcserv /drivers/q8b/hid_keybd     /dev/keyb0  /dev/usb0
@/bin/ipcserv /drivers/q8b/hid_moused    /dev/mouse0 /dev/usb0
@/bin/ipcserv /drivers/q8b/hid_touchd    /dev/touch0 /dev/usb0
@/bin/ipcserv /drivers/q8b/btd           /dev/bt0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/q8b/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /drivers/xserverd          /dev/x
@/bin/bgrun /sbin/x/xtouch
@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /bin/x/xsession misa
