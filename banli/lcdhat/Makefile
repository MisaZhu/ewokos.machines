export MF=banli

all: 
	mkdir -p build
	cd drivers/gamekbd; make
	cd drivers/lcdhatd; make
	cd drivers/xpt2046d; make
	cp -r etc build

clean:	
	cd drivers/gamekbd; make clean
	cd drivers/lcdhatd; make clean
	cd drivers/xpt2046d; make clean
	rm -fr build
