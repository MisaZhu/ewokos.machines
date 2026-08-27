all: basic_libs
	@echo "all done."

DIRS = arch_rk3506 bsp

# bsp includes headers installed by arch_rk3506 into the SDK include dir
bsp: arch_rk3506

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
