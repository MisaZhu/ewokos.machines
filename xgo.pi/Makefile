export MF=xgo

all: 
	mkdir -p build
	cd lib; make
	cd drivers/spilcdd; make
	cd drivers/buttond; make
	cd drivers/soundd; make
	cd apps/xgo; make
	cp -r etc build

clean:	
	cd drivers/spilcdd; make clean
	cd drivers/buttond; make clean
	cd drivers/soundd; make clean
	cd lib; make clean
	cd apps/xgo; make clean
	rm -r build
