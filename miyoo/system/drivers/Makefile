DIRS = audio fbdisplayd g2dd gpio_joystickd ms_uartd
all: 
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = audio fbdisplayd g2dd gpio_joystickd ms_uartd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
