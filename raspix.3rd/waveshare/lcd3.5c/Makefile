export MF=waveshare
include ../../make.inc

all: 
	mkdir -p build
	cd drivers/rpi_lcdd; make
	cp -r etc build

clean:	
	cd drivers/rpi_lcdd; make clean
	rm -fr build
