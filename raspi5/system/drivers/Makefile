BASIC_DIRS = uartd fand cpud nvmefsd
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

network:
	cd wlan; $(MAKE)

GUI_DIRS = fbdisplayd dsi_fbdisplayd usbhostd hid_keybd hid_moused hid_touchd
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = uartd fand cpud nvmefsd fbdisplayd dsi_fbdisplayd usbhostd hid_keybd hid_moused hid_touchd wlan
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
