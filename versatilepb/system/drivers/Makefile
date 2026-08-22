BASIC_DIRS = ttyd powerd ps2keybd smc91c111d
basic: 
	@for dir in $(BASIC_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

GUI_DIRS = fbdisplayd ps2moused
gui:
	@for dir in $(GUI_DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = ttyd powerd fbdisplayd ps2keybd ps2moused smc91c111d
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
