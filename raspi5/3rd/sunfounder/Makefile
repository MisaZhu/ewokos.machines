export MF=sunfounder

DIRS = drivers/i2cdisplayd drivers/spilightd bin apps/xsunfd
all:
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
