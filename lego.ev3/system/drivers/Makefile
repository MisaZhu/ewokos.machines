DIRS = fbdisplayd uartd gpio_joystickd adcd

all: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean $(DIRS)
