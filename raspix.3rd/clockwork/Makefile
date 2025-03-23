export MF=clockwork

all: 
	mkdir -p build
	cd drivers/fbd; make
	cd drivers/usbd; make
	cd drivers/powerd; make
	cp -r etc build/

clean:	
	cd drivers/fbd; make clean
	cd drivers/usbd; make clean
	cd drivers/powerd; make clean
	rm -fr build
