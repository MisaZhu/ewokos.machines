# Q8B: serial console + GOP framebuffer console.
#
# Bring up core services and serial logging, then GPU rendering and the
# display stack. The xwin profile adds USB input and the desktop.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/nulld             /dev/null

@/bin/ipcserv /drivers/q8b/uartd         /dev/tty0
@/bin/shell /etc/g2d.rd
#@/bin/ipcserv /drivers/q8b/fand          /dev/fan

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/q8b/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd
@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
@/bin/bgrun /bin/session -r -t /dev/console0
