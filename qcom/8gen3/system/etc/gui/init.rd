# Q8B: serial console + GOP framebuffer console.
#
# Bring up core services and serial logging, then GPU rendering and the
# display stack. The xwin profile adds USB input and the desktop.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/nulld             /dev/null

@/bin/ipcserv /drivers/8gen3/uartd         /dev/tty0
# The persistent root owns PCIe power, resources and the device snapshot.
@/bin/ipcserv /drivers/8gen3/pcied         /dev/pcie0
@export ADRENO_ENABLE=1
@/bin/ipcserv /drivers/8gen3/g2dd          /dev/g2d
#@/bin/ipcserv /drivers/8gen3/fand          /dev/fan

@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/8gen3/fbdisplayd    /dev/disp0
@/bin/ipcserv /drivers/fontd
@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
@/bin/bgrun /bin/session -r -t /dev/console0
