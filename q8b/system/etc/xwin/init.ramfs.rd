# Q8B boot RAMFS profile.  /sbin/init runs this before selecting the
# persistent rootfs; the normal profile below runs after that switch.
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
