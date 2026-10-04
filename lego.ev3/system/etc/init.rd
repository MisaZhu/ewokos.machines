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
# type matches, several of them can run at once without colliding. All four
# ports auto-detect an I2C (NXT) sensor from a UART (EV3) one: ports 1/2 are
# hardware 16550 UARTs, ports 3/4 are the PRU0 soft-UART, and a UART daemon
# skips any port conn_type names as I2C/analog so it never drives UART framing
# onto an I2C sensor's lines.
@/bin/ipcserv /drivers/ev3/touchd         /dev/touch0
@/bin/ipcserv /drivers/ev3/ultrasonicd    /dev/us0
@/bin/ipcserv /drivers/ev3/gyrod          /dev/gyro0
@/bin/ipcserv /drivers/ev3/colord         /dev/color0
@/bin/ipcserv /drivers/ev3/ird            /dev/ir0
# i2cd is a RAW bit-banged GPIO-I2C bus, not a sensor, and it must NOT sit on a
# PRU soft-UART port. Ports 3/4's pin 5/6 ARE the McASP serialiser lines PRU0
# clocks, so opening them as GPIO here fights the soft-UART that the daemons
# above bring up (their probe walks all four ports) and storms PRU_EVTOUT
# (IRQ 5/6): the boot then hangs right here, waiting for /dev/i2c2 that never
# registers. Start it manually on a free NON-PRU port only when a raw bus is
# wanted, e.g.  /bin/ipcserv /drivers/ev3/i2cd -p 1 /dev/i2c0
#@/bin/ipcserv /drivers/ev3/i2cd     -p 3  /dev/i2c2

# Alternatives, also auto-detecting their port:
#   @/bin/ipcserv /drivers/ev3/touchd       -n    /dev/touch1      (NXT touch)
#   @/bin/ipcserv /drivers/ev3/nxt-ultrasonicd    /dev/nxt-us0     (NXT I2C sensor)

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
