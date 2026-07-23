export MF=xgo

all: 
	mkdir -p build 
	cd lib; $(MAKE)
	cd drivers/spilcdd; $(MAKE)
	cd drivers/buttond; $(MAKE)
	cd drivers/soundd; $(MAKE)
	cd drivers/micd; $(MAKE)
	cd apps/xgo; $(MAKE)
	cd apps/cam; $(MAKE)
	cd apps/mic; $(MAKE)
	cp -r etc build 

clean:	
	cd drivers/spilcdd; $(MAKE) clean
	cd drivers/buttond; $(MAKE) clean
	cd drivers/soundd; $(MAKE) clean
	cd drivers/micd; $(MAKE) clean
	cd lib; $(MAKE) clean
	cd apps/xgo; $(MAKE) clean
	cd apps/cam; $(MAKE) clean
	cd apps/mic; $(MAKE) clean
