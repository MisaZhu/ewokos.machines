export MF=waveshare

all: 
	mkdir -p build
	cd drivers/xpt2046d; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/xpt2046d; $(MAKE) clean
	rm -fr build

