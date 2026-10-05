DIRS = fbdisplayd uartd gpio_keybd adcd motord touchd ultrasonicd gyrod colord ird ledd beepd batteryd i2cd nxt-ultrasonicd

all: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean $(DIRS)
