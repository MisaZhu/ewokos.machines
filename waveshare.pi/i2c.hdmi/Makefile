export MF=waveshare

all: 
	mkdir -p build
	cd drivers/gt911_touchd; make
	cp -r etc build

clean:	
	cd drivers/gt911_touchd; make clean
	rm -fr build
