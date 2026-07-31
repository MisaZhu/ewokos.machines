export MF=waveshare

all: 
	mkdir -p build
	cd drivers/ili9486d; $(MAKE)
	cd drivers/spi2uartd; $(MAKE)

clean:	
	cd drivers/ili9486d; $(MAKE) clean
	cd drivers/spi2uartd; $(MAKE) clean
	rm -fr build
