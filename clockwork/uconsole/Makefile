export MF=clockwork

all: 
	mkdir -p build
	cd drivers/fbdisplayd; $(MAKE)
	cd drivers/fbdisplay6d; $(MAKE)
	cd drivers/powerd; $(MAKE)
	cd drivers/soundpwmd; $(MAKE)
	cp -r etc build/

clean:	
	cd drivers/fbdisplayd; $(MAKE) clean
	cd drivers/fbdisplay6d; $(MAKE) clean
	cd drivers/powerd; $(MAKE) clean
	cd drivers/soundpwmd; $(MAKE) clean
	rm -fr build
