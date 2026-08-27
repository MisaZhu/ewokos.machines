DIRS = audio fbdisplayd g2dd gpio_joystickd ms_uartd
all: 
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:	
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
