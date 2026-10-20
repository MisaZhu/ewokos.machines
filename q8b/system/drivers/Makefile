BASIC_DIRS = uartd net sdprobe fand sdupdate

basic: $(BASIC_DIRS)

GUI_DIRS = fbdisplayd

gui: $(GUI_DIRS)

$(BASIC_DIRS) $(GUI_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic gui clean $(BASIC_DIRS) $(GUI_DIRS)
