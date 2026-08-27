BASIC_DIRS = uartd fand cpud nvmefsd
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

NETWORK_DIRS = wlan
network:
	cd wlan; $(MAKE)

GUI_DIRS = fbdisplayd dsi_fbdisplayd
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = $(NETWORK_DIRS) $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
