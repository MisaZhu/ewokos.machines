export MF=waveshare

all: 
	mkdir -p build
	cd drivers/lcdhatd; make
	cd drivers/keybd; make
	cp -r etc build

clean:	
	cd drivers/lcdhatd; make clean
	cd drivers/keybd; make clean
	rm -fr build
