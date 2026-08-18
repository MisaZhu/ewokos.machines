export MF=waveshare

all:
	mkdir -p build
	cd drivers/dsi_touchd; $(MAKE)
	cp -r etc build

clean:
	cd drivers/dsi_touchd; $(MAKE) clean
	rm -fr build
