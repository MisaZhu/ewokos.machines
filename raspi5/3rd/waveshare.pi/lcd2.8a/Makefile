export MF=waveshare

all: 
	mkdir -p build
	cd drivers/lcdhatd; $(MAKE)
	cd drivers/keybd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/lcdhatd; $(MAKE) clean
	cd drivers/keybd; $(MAKE) clean
	rm -fr build
