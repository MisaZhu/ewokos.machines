export MF=xgo

all: 
	mkdir -p build 
	cd lib; $(MAKE)
	cd drivers/spilcdd; $(MAKE)
	cd drivers/buttond; $(MAKE)
	cd drivers/soundd; $(MAKE)
	cd apps/xgo; $(MAKE)
	cp -r etc build 

clean:	
	cd drivers/spilcdd; $(MAKE) clean
	cd drivers/buttond; $(MAKE) clean
	cd drivers/soundd; $(MAKE) clean
	cd lib; $(MAKE) clean
	cd apps/xgo; $(MAKE) clean
