@/bin/ipcserv /drivers/8gen3/uartd         /dev/tty0
# The persistent root owns PCIe power, resources and the device snapshot.
@/bin/ipcserv /drivers/8gen3/pcied         /dev/pcie0
#@/bin/ipcserv /drivers/8gen3/fand          /dev/fan
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped           /dev/pipe0
@/bin/ipcserv /drivers/ramfsd          /tmp
@/bin/ipcserv /drivers/nulld           /dev/null
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0
