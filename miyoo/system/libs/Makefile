all: basic_libs
	@echo "all done."

DIRS = arch_miyoo bsp

# bsp includes headers installed by arch_miyoo into the SDK include dir
bsp: arch_miyoo

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
