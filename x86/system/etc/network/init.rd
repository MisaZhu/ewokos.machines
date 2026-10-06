#@/bin/ipcserv /drivers/x86/ttyd /dev/tty0

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped  /dev/pipe0
@/bin/ipcserv /drivers/ramfsd /tmp
@/bin/ipcserv /drivers/nulld /dev/null

@/bin/ipcserv /drivers/x86/net /dev/eth0
@/bin/ipcserv /drivers/netd /dev/net0 /dev/eth0
@/bin/ipcserv /drivers/timed    /dev/time

# Intel WLAN (BE202 / AX211 / AX411): to use wifi instead of the wired
# NIC, comment the two wired lines above and uncomment these:
#@/bin/ipcserv /drivers/x86/wland /dev/wl0
#@/bin/ipcserv /drivers/netd /dev/net0 /dev/wl0

@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /sbin/telnetd
@/bin/bgrun /sbin/sshd

#@/bin/bgrun /bin/session -r -t /dev/tty0
