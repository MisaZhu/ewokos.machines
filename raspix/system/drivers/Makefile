BASIC_DIRS = uartd cpud soundd camd 
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

GUI_DIRS = fbdisplayd dsi_fbdisplayd
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

NETWORK_DIRS = wlan btd
network:
	@for dir in $(NETWORK_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS) $(NETWORK_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	@for dir in $(USB_STACK_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
