BASIC_DIRS = uartd cpud soundd ../../3rd/gnpe/lcdhat2.4.touch/drivers/soundpwmd usbhostd hid_keybd hid_joystickd camd
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

GUI_DIRS = fbdisplayd dsi_fbdisplayd g2dd hid_moused hid_touchd
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

NETWORK_DIRS = wlan btd
network:
	@for dir in $(NETWORK_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = uartd cpud fbdisplayd dsi_fbdisplayd g2dd soundd ../../3rd/gnpe/lcdhat2.4.touch/drivers/soundpwmd usbhostd hid_keybd hid_moused hid_touchd hid_joystickd wlan btd camd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
