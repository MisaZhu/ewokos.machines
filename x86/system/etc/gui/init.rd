@/bin/ipcserv /drivers/x86/ttyd /dev/tty0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/tty0

@/bin/ipcserv /drivers/displayd        
@/bin/ipcserv /drivers/x86/fbd      /dev/fb0
@/bin/ipcserv /drivers/fontd           

@/bin/ipcserv /drivers/consoled        -i /dev/keyb0
@set_stdio /dev/console0

@/bin/ipcserv /drivers/timerd          
@/bin/ipcserv /drivers/ramfsd          /tmp
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/ipcserv /drivers/x86/ps2keybd   /dev/keyb0
@/bin/ipcserv /drivers/x86/ps2moused   /dev/mouse0

@/bin/bgrun /bin/session -r -t /dev/console0
