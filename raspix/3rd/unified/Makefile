all: 
	mkdir -p build
	cd drivers/xpt2046d; $(MAKE)
	cd drivers/gt911_touchd; $(MAKE)

clean:	
	cd drivers/xpt2046d; $(MAKE) clean
	cd drivers/gt911_touchd; $(MAKE) clean
	rm -fr build
