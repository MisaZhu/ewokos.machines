@/bin/ipcserv /drivers/raspi5/uartd         /dev/tty0
@/bin/ipcserv /drivers/raspi5/cpud          /dev/cpu
@set_stdio /dev/tty0

# take the cooling fan over from the EEPROM bootloader as early as possible
@/bin/ipcserv /drivers/raspi5/fand          /dev/fan

@/bin/ipcserv /drivers/timerd

@/bin/ipcserv /drivers/ramfsd          /tmp
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
