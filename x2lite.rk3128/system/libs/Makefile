all: basic_libs
	@echo "all done."

DIRS = arch_rk3128 bsp

# bsp includes headers installed by arch_rk3128 into the SDK include dir
bsp: arch_rk3128

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
