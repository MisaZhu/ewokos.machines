BASIC_DIRS = uartd fand cpud nvmefsd
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

network:
	cd wlan; $(MAKE)

GUI_DIRS = fbdisplayd dsi_fbdisplayd g2dd
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = wlan $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
