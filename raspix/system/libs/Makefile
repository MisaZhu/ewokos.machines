all: basic_libs
	@echo "all done."

DIRS = arch_bcm283x bsp

# bsp includes headers installed by arch_bcm283x into the SDK include dir
bsp: arch_bcm283x

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
