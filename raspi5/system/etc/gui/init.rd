@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/raspi5/uartd         /dev/tty0
# take the cooling fan over from the EEPROM bootloader as early as possible
@/bin/ipcserv /drivers/raspi5/fand          /dev/fan
@/bin/ipcserv /drivers/raspi5/cpud          /dev/cpu
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/raspi5/fbd      /dev/fb0
#@/bin/ipcserv /drivers/raspi5/gt911_touchd /dev/touch0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd          /tmp
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/ipcserv /drivers/raspi5/usbhostd    /dev/hid0
@/bin/ipcserv /drivers/raspi5/hid_keybd   /dev/keyb0  /dev/hid0
@/bin/ipcserv /drivers/raspi5/hid_moused  /dev/mouse0 /dev/hid0
# GT911 owns /dev/touch0; use another mount point if USB touch is enabled.
#@/bin/ipcserv /drivers/raspi5/hid_touchd  /dev/touch1 /dev/hid0

@/bin/bgrun /bin/session -r -t /dev/console0
