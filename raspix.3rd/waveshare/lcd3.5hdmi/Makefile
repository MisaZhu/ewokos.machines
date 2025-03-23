export MF=waveshare

all: 
	mkdir -p build
	cd drivers/xpt2046d; make
	cp -r etc build

clean:	
	cd drivers/xpt2046d; make clean
	rm -fr build

