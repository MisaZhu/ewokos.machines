# Q8B: serial console + GOP framebuffer console.
#
# Bring up the core services first, then the display stack, then one login
# session per console.  There is no input driver yet: the framebuffer session
# is visible but only the serial session accepts keyboard input.  USB
# xHCI/HID is the next milestone.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/nulld             /dev/null

@/bin/ipcserv /drivers/q8b/uartd         /dev/tty0
#@/bin/ipcserv /drivers/q8b/fand          /dev/fan

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/q8b/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd
@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
@/bin/bgrun /bin/session -r -t /dev/console0
