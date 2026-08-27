DIRS = audio fbdisplayd g2dd gpio_joystickd ms_uartd

all: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean $(DIRS)
