export MF=clockwork

all: 
	mkdir -p build
	cd drivers/fbd; $(MAKE)
	cd drivers/fb6d; $(MAKE)
	cd drivers/powerd; $(MAKE)
	cd drivers/soundpwmd; $(MAKE)
	cp -r etc build/

clean:	
	cd drivers/fbd; $(MAKE) clean
	cd drivers/fb6d; $(MAKE) clean
	cd drivers/powerd; $(MAKE) clean
	cd drivers/soundpwmd; $(MAKE) clean
	rm -fr build
