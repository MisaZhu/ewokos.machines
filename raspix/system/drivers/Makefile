basic:
	cd uartd; $(MAKE)
	cd soundd; $(MAKE)
	cd usbd; $(MAKE)
	cd usbd.touch; $(MAKE)
	cd hid_keybd; $(MAKE)
	cd hid_joystickd; $(MAKE)
	cd spi2uart; $(MAKE)

gui:
	cd fbd; $(MAKE)
	cd dsi_fbd; $(MAKE)
	cd hid_moused; $(MAKE)

network:
	cd wlan; $(MAKE)
	cd btd; $(MAKE)

clean:	
	cd uartd; $(MAKE) clean
	cd fbd; $(MAKE) clean
	cd dsi_fbd; $(MAKE) clean
	cd soundd; $(MAKE) clean
	cd usbd; $(MAKE) clean
	cd usbd.touch; $(MAKE) clean
	cd hid_keybd; $(MAKE) clean
	cd hid_moused; $(MAKE) clean
	cd hid_joystickd; $(MAKE) clean
	cd spi2uart; $(MAKE) clean
	cd wlan; $(MAKE) clean
	cd btd; $(MAKE) clean
