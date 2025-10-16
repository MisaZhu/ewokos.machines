export MF=raspix

all: 
	mkdir -p build
	cd drivers/xpt2046d; make
	cd drivers/gt911_touchd; make

clean:	
	cd drivers/xpt2046d; make clean
	cd drivers/gt911_touchd; make clean
	rm -fr build
