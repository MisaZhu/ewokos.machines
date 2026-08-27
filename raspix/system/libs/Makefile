all: basic_libs
	@echo "all done."

DIRS = arch_bcm283x bsp
basic_libs:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
