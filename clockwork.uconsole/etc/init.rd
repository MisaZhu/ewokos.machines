@/bin/ipcserv /drivers/clockwork/powerd      /dev/power0
@/bin/ipcserv /drivers/clockwork/fbd         /dev/fb0
@/bin/ipcserv /drivers/displayd              
@/bin/ipcserv /drivers/fontd                 

@export UX_ID=0
@/bin/ipcserv /drivers/consoled              /dev/klog
@set_stdio /dev/klog
@export KLOG_DEV=/dev/klog

@/bin/ipcserv /drivers/raspix/uartd          /dev/tty0
@/bin/ipcserv /drivers/timerd                

@/bin/ipcserv /drivers/clockwork/usbd        /dev/hid0
@/bin/ipcserv /drivers/raspix/hid_keybd      /dev/keyb0
@/bin/ipcserv /drivers/raspix/hid_moused     /dev/mouse0

@/bin/ipcserv /drivers/ramfsd                /tmp
@/bin/ipcserv /drivers/nulld                 /dev/null

#@/bin/ipcserv /drivers/raspix/wland          /dev/wl0
#@/bin/ipcserv /drivers/netd                  /dev/net0 /dev/wl0
#@/bin/bgrun /bin/telnetd

@/bin/ipcserv /sbin/sessiond

@export UX_ID=1
@/bin/ipcserv /drivers/consoled              /dev/console1 -i /dev/keyb0
@/bin/bgrun /bin/session -r -t /dev/console1 

@export UX_ID=2
@/bin/ipcserv /drivers/consoled              /dev/console2 -i /dev/keyb0
@/bin/bgrun /bin/session -r -t /dev/console2 

#@/bin/load_font

@/bin/bgrun /sbin/x/xmouse /dev/mouse0 
@/bin/bgrun /sbin/x/xim_none 

@/bin/ipcserv /drivers/xserverd              /dev/x

@/bin/bgrun /bin/x/xsession misa 
