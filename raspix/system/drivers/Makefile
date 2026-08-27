BASIC_DIRS = uartd cpud soundd camd
basic: $(BASIC_DIRS)

GUI_DIRS = fbdisplayd dsi_fbdisplayd
gui: $(GUI_DIRS)

NETWORK_DIRS = wlan btd
network: $(NETWORK_DIRS)

$(BASIC_DIRS) $(GUI_DIRS) $(NETWORK_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS) $(NETWORK_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic gui network clean $(BASIC_DIRS) $(GUI_DIRS) $(NETWORK_DIRS)
