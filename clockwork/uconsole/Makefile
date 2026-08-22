export MF=clockwork

DIRS = drivers/fbdisplayd drivers/fbdisplay6d drivers/powerd drivers/soundpwmd
all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build/

CLEAN_DIRS = drivers/fbdisplayd drivers/fbdisplay6d drivers/powerd drivers/soundpwmd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
