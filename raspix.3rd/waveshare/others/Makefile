export MF=waveshare
include ../../make.inc

all: 
	mkdir -p build
	cd drivers/ili9486d; make

clean:	
	cd drivers/ili9486d; make clean
	rm -fr build

