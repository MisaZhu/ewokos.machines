BASIC_DIRS = uartd fand cpud nvmefsd
basic: $(BASIC_DIRS)

NETWORK_DIRS = wlan
network: $(NETWORK_DIRS)

GUI_DIRS = fbdisplayd dsi_fbdisplayd
gui: $(GUI_DIRS)

$(BASIC_DIRS) $(NETWORK_DIRS) $(GUI_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(NETWORK_DIRS) $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic network gui clean $(BASIC_DIRS) $(NETWORK_DIRS) $(GUI_DIRS)
