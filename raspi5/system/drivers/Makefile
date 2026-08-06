basic:
	cd uartd; $(MAKE)

network:
	# TODO: wlan, btd drivers

gui:
	cd fbd; $(MAKE)
	cd gt911_touchd; $(MAKE)
	# TODO: g2dd, hid_moused, hid_touchd drivers

clean:
	cd uartd; $(MAKE) clean
	cd fbd; $(MAKE) clean
	cd gt911_touchd; $(MAKE) clean
