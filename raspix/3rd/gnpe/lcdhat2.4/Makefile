export HW=raspix
export MF=gnpe

all: 
	mkdir -p build
	cd drivers/lcdhatd; $(MAKE)
	cp -r etc build

clean:	
	cd drivers/lcdhatd; $(MAKE) clean
	rm -fr build
