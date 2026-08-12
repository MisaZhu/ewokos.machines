export MF=sunfounder

all:
	mkdir -p build
	cd drivers/i2cfbd; $(MAKE)
	cp -r etc build

clean:
	cd drivers/i2cfbd; $(MAKE) clean
	rm -fr build
