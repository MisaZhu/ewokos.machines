export MF=waveshare

all: 
	mkdir -p build
	cd drivers/lcdhatd; make
	cp -r etc build

clean:	
	cd drivers/lcdhatd; make clean
	rm -fr build
