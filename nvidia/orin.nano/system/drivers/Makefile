BASIC_DIRS = uartd net pcied i2cd orinmon gpiod sdprobe bpmpdiag

basic: $(BASIC_DIRS)

$(BASIC_DIRS):
	@$(MAKE) -C $@

CLEAN_DIRS = $(BASIC_DIRS)
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: basic clean $(BASIC_DIRS)