@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/rk3128/rk_uartd       /dev/tty0

@/bin/ipcserv /drivers/displayd             
@/bin/ipcserv /drivers/rk3128/fbd            /dev/fb0
@/bin/ipcserv /drivers/fontd                
@/bin/load_font

@export UX_ID=0
@/bin/ipcserv /drivers/consoled             /dev/console0
@set_stdio /dev/console0

@/bin/ipcserv /drivers/rk3128/gpio_joystickd  /dev/joystick
@/bin/ipcserv /drivers/vkeybd                 /dev/vkeyb /dev/joystick -t j
@/bin/ipcserv /drivers/vjoystickd             /dev/vjoystick /dev/vkeyb -m

@/bin/ipcserv /drivers/timerd               
@/bin/ipcserv /drivers/nulld                /dev/null
@/bin/ipcserv /drivers/ramfsd               /tmp

@/bin/ipcserv /sbin/sessiond
#@/bin/bgrun /bin/session -r 

@/bin/bgrun /sbin/x/xim_none   /dev/vjoystick 
@/bin/bgrun /sbin/x/xmouse     /dev/vjoystick 

@/bin/ipcserv /drivers/xserverd             /dev/x
@/bin/bgrun /sbin/x/xim_vkey -w 560 -h 168
@/bin/bgrun /bin/x/xsession misa 
