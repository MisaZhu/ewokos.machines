DIRS = fbdisplayd gpio_joystickd rk_uartd
all: 
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = fbdisplayd gpio_joystickd rk_uartd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
