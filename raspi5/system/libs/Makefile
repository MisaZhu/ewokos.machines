all: basic_libs
	@echo "all done."

DIRS = arch_bcm2712 bsp

# bsp includes headers installed by arch_bcm2712 into the SDK include dir
bsp: arch_bcm2712

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
