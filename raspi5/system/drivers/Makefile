basic:
	cd uartd; $(MAKE)

network:
	# TODO: wlan, btd drivers

gui:
	cd fbd; $(MAKE)
	cd usbhostd; $(MAKE)
	cd hid_keybd; $(MAKE)
	cd hid_moused; $(MAKE)
	cd hid_touchd; $(MAKE)
	cd gt911_touchd; $(MAKE)

clean:
	cd uartd; $(MAKE) clean
	cd fbd; $(MAKE) clean
	cd usbhostd; $(MAKE) clean
	cd hid_keybd; $(MAKE) clean
	cd hid_moused; $(MAKE) clean
	cd hid_touchd; $(MAKE) clean
	cd gt911_touchd; $(MAKE) clean
