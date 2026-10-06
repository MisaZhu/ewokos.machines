BASIC_DIRS = ttyd powerd nvmefsd atafsd vgacond 
basic: $(BASIC_DIRS)

GUI_DIRS = fbdisplayd ps2keybd ps2moused
gui: $(GUI_DIRS)

NET_DIRS = net wlan
network: $(NET_DIRS)

$(BASIC_DIRS) $(GUI_DIRS) $(NET_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS) $(NET_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic gui network clean $(BASIC_DIRS) $(GUI_DIRS) $(NET_DIRS)
