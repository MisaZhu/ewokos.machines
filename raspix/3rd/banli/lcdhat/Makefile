export MF=banli

DIRS = drivers/gamekbd drivers/lcdhatd
all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build

CLEAN_DIRS = drivers/gamekbd drivers/lcdhatd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
