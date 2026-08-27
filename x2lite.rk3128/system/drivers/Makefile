DIRS = fbdisplayd gpio_joystickd rk_uartd
all: 
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:	
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
