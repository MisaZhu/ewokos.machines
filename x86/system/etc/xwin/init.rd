@/bin/ipcserv /drivers/x86/ttyd /dev/tty0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displayd
@/bin/ipcserv /drivers/x86/fbd /dev/fb0
@/bin/ipcserv /drivers/fontd

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/nulld /dev/null
@/bin/ipcserv /drivers/ramfsd /tmp

@/bin/ipcserv /drivers/x86/keybd /dev/keyb0
@/bin/ipcserv /drivers/x86/moused /dev/mouse0

#@/bin/load_font

@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/bgrun /sbin/x/xmouse
@/bin/bgrun /sbin/x/xim_none
@/bin/bgrun /bin/x/xsession  misa
