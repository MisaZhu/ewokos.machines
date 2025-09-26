@/bin/ipcserv /drivers/displayd        
@/bin/ipcserv /drivers/ev3/fbd      /dev/fb0
@/bin/ipcserv /drivers/fontd           
@/bin/load_font

@export UX_ID=0
@/bin/ipcserv /drivers/consoled        -u 0
@set_stdio /dev/console0

@/bin/ipcserv /drivers/ev3/gpio_joystickd     /dev/joystick
@/bin/ipcserv /drivers/ev3/adcd     /dev/adc0

@/bin/ipcserv /drivers/timerd         

@/bin/ipcserv /sbin/sessiond

@/bin/ipcserv /drivers/xserverd       /dev/x
@/bin/bgrun /sbin/x/xim_none   /dev/joystick -t 30000
@/bin/bgrun /sbin/x/xim_vkey -w 178 -h 60

@/bin/bgrun /bin/x/xsession misa 
