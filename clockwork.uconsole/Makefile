export MF=clockwork

all: 
	mkdir -p build
	cd drivers/fbd; $(MAKE)
	cd drivers/powerd; $(MAKE)
	cp -r etc build/

clean:	
	cd drivers/fbd; $(MAKE) clean
	cd drivers/powerd; $(MAKE) clean
	rm -fr build
