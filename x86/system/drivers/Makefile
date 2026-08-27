BASIC_DIRS = ttyd powerd
basic:
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

GUI_DIRS = fbdisplayd ps2keybd ps2moused
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

network:

CLEAN_DIRS = $(BASIC_DIRS) $(GUI_DIRS) 
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
