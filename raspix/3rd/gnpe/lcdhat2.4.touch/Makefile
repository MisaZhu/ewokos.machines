export HW=raspix
export MF=gnpe

all: 
	mkdir -p build
	cd drivers/kbd; make
	cd drivers/lcdhatd; make
	cd drivers/soundpwmd; make
	cp -r etc build

clean:	
	cd drivers/kbd; make clean
	cd drivers/lcdhatd; make clean
	cd drivers/soundpwmd; make clean
	rm -fr build
