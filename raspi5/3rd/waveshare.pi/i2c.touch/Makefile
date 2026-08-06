export MF=waveshare

all: 
	mkdir -p build
	cd drivers/gt911_touchd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/gt911_touchd; $(MAKE) clean
	rm -fr build
