BASIC_DIRS = ttyd powerd
basic: $(BASIC_DIRS)

GUI_DIRS = fbdisplayd ps2keybd ps2moused
gui: $(GUI_DIRS)

network:

$(BASIC_DIRS) $(GUI_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic gui network clean $(BASIC_DIRS) $(GUI_DIRS)
