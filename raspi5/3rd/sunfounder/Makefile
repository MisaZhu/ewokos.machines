export MF=sunfounder

all:
	mkdir -p build
	cd drivers/i2cfbd; $(MAKE)
	cd bin; $(MAKE)
	cp -r etc build

clean:
	cd drivers/i2cfbd; $(MAKE) clean
	cd bin; $(MAKE) clean
	rm -fr build
