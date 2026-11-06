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
# TW220 uses the SPI-connected Goodix GT9916 controller.  Keep a single
# owner of /dev/touch0; the USB HID daemon must stay disabled on this panel.
#@/bin/ipcserv /drivers/q8b/hid_touchd    /dev/touch0 /dev/usb0
@/bin/ipcserv /drivers/q8b/goodixd        /dev/touch0
@/bin/ipcserv /drivers/q8b/hid_touchpadd /dev/touchpad0 /dev/usb0
# btd may be launched concurrently by ipcserv, but the Q8B transport gates
# USB Bluetooth setup until wlan has completed WCN startup and published MAC.
# The device surface and the subscribers are the raspi5 ones: btd owns
# /dev/bt0, xbt and devcmd use it, and the shared hid_* daemons register as
# "bt" subscribers. Their nodes exist before a device is paired, so the X
# input processes below can open them at boot.
#@/bin/ipcserv /drivers/q8b/btd           /dev/bt0
#@/bin/ipcserv /drivers/q8b/hid_moused    /dev/mouse1 /dev/bt0 bt
#@/bin/ipcserv /drivers/q8b/hid_keybd     /dev/keyb1  /dev/bt0 bt
#@/bin/ipcserv /drivers/q8b/hid_joystickd /dev/js1    /dev/bt0 bt

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/q8b/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /drivers/xserverd          /dev/x
@/bin/bgrun /sbin/x/xtouch -c /etc/x/xtouch-tw220.json /dev/touch0
@/bin/bgrun /sbin/x/xtouch -m
@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xmouse /dev/mouse1
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /sbin/x/xim_none /dev/keyb1
@/bin/bgrun /sbin/x/xim_none /dev/js1
@/bin/bgrun /bin/x/xsession misa
