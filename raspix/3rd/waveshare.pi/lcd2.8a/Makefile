export MF=waveshare

DIRS = drivers/lcdhatd drivers/keybd
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
