@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displaymand        
@/bin/ipcserv /drivers/ev3/fbdisplayd      /dev/disp0
@/bin/ipcserv /drivers/fontd           

@/bin/ipcserv /drivers/consoled        -u 0
@set_stdio /dev/console0

@/bin/load_font

@/bin/ipcserv /drivers/ev3/gpio_joystickd     /dev/joystick
@/bin/ipcserv /drivers/ev3/adcd     /dev/adc0

@/bin/ipcserv /drivers/ev3/motord        /dev/motor
# Input ports: one daemon per port. Analog (touchd), UART (ultrasonicd/
# gyrod/colord/ird) and I2C (i2cd/nxt-ultrasonicd) all share pin 5/6, so
# never start two of them on the same port. Only ports 1 and 2 have a
# hardware UART (3/4 are PRU soft-UART, not implemented here); GPIO I2C
# works on any port.
@/bin/ipcserv /drivers/ev3/touchd        -p 1  /dev/touch0
@/bin/ipcserv /drivers/ev3/ultrasonicd   -p 2  /dev/us0
@/bin/ipcserv /drivers/ev3/i2cd          -p 3  /dev/i2c2
# Alternatives, start manually on a free port:
#   @/bin/ipcserv /drivers/ev3/gyrod  -p 2  /dev/gyro0
#   @/bin/ipcserv /drivers/ev3/colord -p 2  /dev/color0
#   @/bin/ipcserv /drivers/ev3/ird    -p 2  /dev/ir0
#   @/bin/ipcserv /drivers/ev3/touchd -n -p 4  /dev/touch1      (NXT touch)
#   @/bin/ipcserv /drivers/ev3/nxt-ultrasonicd -p 4  /dev/nxt-us0
@/bin/ipcserv /drivers/ev3/ledd          /dev/led
@/bin/ipcserv /drivers/ev3/beepd         /dev/beep
@/bin/ipcserv /drivers/ev3/batteryd      /dev/battery
@/bin/ipcserv /drivers/ramfsd       /tmp         
@/bin/ipcserv /drivers/piped         
@/bin/ipcserv /drivers/timerd         

@/bin/ipcserv /sbin/sessiond

@/bin/bgrun /sbin/x/xim_none   /dev/joystick -t 30000
@/bin/bgrun /sbin/x/xim_vkey -w 178 -h 60

@/bin/ipcserv /drivers/xserverd       /dev/x
@/bin/bgrun /bin/x/xsession misa 
