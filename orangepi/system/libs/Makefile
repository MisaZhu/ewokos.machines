all: basic_libs
	@echo "all done."

DIRS = arch_orangepi bsp

# bsp includes headers installed by arch_orangepi into the SDK include dir
bsp: arch_orangepi

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
