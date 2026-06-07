export MF=waveshare

all: 
	mkdir -p build
	cd drivers/joykeybd; $(MAKE)
	cd drivers/gamekbd; $(MAKE)
	cd drivers/lcdhatd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/joykeybd; $(MAKE) clean
	cd drivers/gamekbd; $(MAKE) clean
	cd drivers/lcdhatd; $(MAKE) clean
	rm -fr build
