export MF=sunfounder

DIRS = drivers/i2cdisplayd bin
all:
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build

CLEAN_DIRS = drivers/i2cdisplayd bin
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
