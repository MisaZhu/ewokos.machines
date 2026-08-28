all: basic_libs
	@echo "all done."

DIRS = arch_ev3 bsp

# bsp includes headers installed by arch_ev3 into the SDK include dir
bsp: arch_ev3

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
