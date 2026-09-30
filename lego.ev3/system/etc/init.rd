@/bin/ipcserv /drivers/logd /dev/log

@/bin/ipcserv /drivers/displaymand        
@/bin/ipcserv /drivers/ev3/fbdisplayd      /dev/disp0
@/bin/ipcserv /drivers/fontd           

#@/bin/ipcserv /drivers/consoled
#@set_stdio /dev/console0

#@/bin/load_font

@/bin/ipcserv /sbin/splashd -w 178 -h 128 -f 10
@/bin/splash -i /usr/system/icons/lego.png -m "start..."

@/bin/splash -m "start /dev/keyb" -p 20
@/bin/ipcserv /drivers/ev3/gpio_keybd     /dev/keyb0
@/bin/ipcserv /drivers/vkeybd   /dev/vkeyb    /dev/keyb0

@/bin/splash -m "start /dev/adc0" -p 30
@/bin/ipcserv /drivers/ev3/adcd     /dev/adc0

@/bin/splash -m "start /dev/motor" -p 40
@/bin/ipcserv /drivers/ev3/motord        /dev/motor
@/bin/splash -m "start sensors" -p 55
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

@/bin/splash -m "start /dev/led" -p 65
@/bin/ipcserv /drivers/ev3/ledd          /dev/led
@/bin/ipcserv /drivers/ev3/beepd         /dev/beep
@/bin/ipcserv /drivers/ev3/batteryd      /dev/battery

@/bin/splash -m "mount /tmp" -p 75
@/bin/ipcserv /drivers/ramfsd       /tmp         
#@/bin/ipcserv /drivers/piped         
@/bin/splash -m "start /dev/timer" -p 80
@/bin/ipcserv /drivers/timerd         

@/bin/splash -m "start sessiond" -p 85
@/bin/ipcserv /sbin/sessiond

@/bin/splash -m "start xim" -p 90
@/bin/bgrun /sbin/x/xim_none   /dev/vkeyb -t 30000
@/bin/bgrun /sbin/x/xim_vkey -w 178 -h 60

@/bin/splash -m "startx" -p 100
@/bin/ipcserv /drivers/xserverd       /dev/x
@/bin/bgrun /bin/x/xsession misa 
