BASIC_DIRS = uartd net pcied i2cd orinmon gpiod sdprobe bpmpdiag fand dceprobe
GUI_DIRS = fbdisplayd

basic: $(BASIC_DIRS)

gui: $(GUI_DIRS)

$(BASIC_DIRS) $(GUI_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic gui clean $(BASIC_DIRS) $(GUI_DIRS)