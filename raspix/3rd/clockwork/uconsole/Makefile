export MF=clockwork

DIRS = drivers/fbdisplayd drivers/fbdisplay6d drivers/powerd drivers/soundpwmd
all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build/

clean:	
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
