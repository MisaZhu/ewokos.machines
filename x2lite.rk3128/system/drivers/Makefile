DIRS = fbdisplayd gpio_joystickd rk_uartd

all: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean $(DIRS)
