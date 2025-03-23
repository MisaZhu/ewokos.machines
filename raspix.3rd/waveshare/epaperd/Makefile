export MF=waveshare

all: 
	mkdir -p build
	cd drivers/epaperd; make
	cp -r etc build

clean:	
	cd drivers/epaperd; make clean
	rm -fr build
