@/bin/ipcserv /drivers/raspix/uartd         /dev/tty0

@/bin/ipcserv /drivers/raspix/fbd      /dev/fb0
@/bin/ipcserv /drivers/displayd        
@/bin/ipcserv /drivers/fontd           
@/bin/load_font

@export UX_ID=0
@/bin/ipcserv /drivers/consoled       
@set_stdio /dev/console0
@export KLOG_DEV=/dev/console0

@/bin/ipcserv /drivers/timerd          

@/bin/ipcserv /drivers/waveshare/gt911_touchd  /dev/touch0

@/bin/ipcserv /drivers/ramfsd          /tmp
@/bin/ipcserv /drivers/nulld           /dev/null

@/bin/ipcserv /sbin/sessiond

@/bin/ipcserv /drivers/xserverd        /dev/x

@/bin/bgrun /sbin/x/xtouch 
@/bin/bgrun /sbin/x/xim_vkey -h 168
@/bin/bgrun /bin/x/xsession  misa 
