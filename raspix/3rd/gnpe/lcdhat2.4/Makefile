export HW=raspix
export MF=gnpe

all: 
	mkdir -p build
	cd drivers/kbd; $(MAKE)
	cd drivers/lcdhatd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/kbd; $(MAKE) clean
	cd drivers/lcdhatd; $(MAKE) clean
	rm -fr build
