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
# Sensor daemons no longer take a port: each one scans the input ports for the
# ID voltage of its own sensor type (adcd above publishes the pin 1 channels),
# binds the port it finds and follows the sensor when it is re-plugged elsewhere
# (hot-plug). The bound port is a device property, readable via GET_DATA
# (ev3_sensor_data_t.port). Because a daemon only claims a port whose detected
# type matches, several of them can run at once without colliding. Only ports 1
# and 2 have a hardware UART (3/4 are PRU soft-UART, not implemented), so the
# UART sensors bind just those; analog touch and GPIO-I2C sensors work on any
# port. i2cd is a raw bus (not a sensor) and still selects its port with -p.
@/bin/ipcserv /drivers/ev3/touchd         /dev/touch0
@/bin/ipcserv /drivers/ev3/ultrasonicd    /dev/us0
@/bin/ipcserv /drivers/ev3/gyrod          /dev/gyro0
@/bin/ipcserv /drivers/ev3/colord         /dev/color0
@/bin/ipcserv /drivers/ev3/ird            /dev/ir0
@/bin/ipcserv /drivers/ev3/i2cd     -p 3  /dev/i2c2

# Alternatives, also auto-detecting their port:
#   @/bin/ipcserv /drivers/ev3/touchd       -n    /dev/touch1      (NXT touch)
#   @/bin/ipcserv /drivers/ev3/nxt-ultrasonicd    /dev/nxt-us0     (avoid a port used by i2cd)

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
