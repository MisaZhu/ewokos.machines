@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displaymand        
@/bin/ipcserv /drivers/ev3/fbdisplayd      /dev/disp0
@/bin/ipcserv /drivers/fontd           

@/bin/ipcserv /drivers/consoled        -u 0
@set_stdio /dev/console0

@/bin/load_font

@/bin/ipcserv /drivers/ev3/gpio_joystickd     /dev/joystick
@/bin/ipcserv /drivers/ev3/adcd     /dev/adc0

@/bin/ipcserv /drivers/ramfsd       /tmp         
@/bin/ipcserv /drivers/piped         
@/bin/ipcserv /drivers/timerd         

@/bin/ipcserv /sbin/sessiond

@/bin/bgrun /sbin/x/xim_none   /dev/joystick -t 30000
@/bin/bgrun /sbin/x/xim_vkey -w 178 -h 60

@/bin/ipcserv /drivers/xserverd       /dev/x
@/bin/bgrun /bin/x/xsession misa 
