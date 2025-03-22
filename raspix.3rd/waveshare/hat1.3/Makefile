export MF=waveshare
include ../../make.inc

all: 
	mkdir -p build
	cd drivers/joykeybd; make
	cd drivers/gamekbd; make
	cd drivers/lcdhatd; make
	cp -r etc build

clean:	
	cd drivers/joykeybd; make clean
	cd drivers/gamekbd; make clean
	cd drivers/lcdhatd; make clean
	rm -fr build
