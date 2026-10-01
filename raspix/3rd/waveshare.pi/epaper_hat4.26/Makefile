export MF=waveshare

all: 
	mkdir -p build
	cd drivers/epaperd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/epaperd; $(MAKE) clean
	rm -fr build
