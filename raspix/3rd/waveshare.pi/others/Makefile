export MF=waveshare

DIRS = drivers/ili9486d drivers/spi2uartd
all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

CLEAN_DIRS = drivers/ili9486d drivers/spi2uartd
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
