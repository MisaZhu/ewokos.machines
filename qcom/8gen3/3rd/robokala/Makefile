all: 
	mkdir -p build
	cd drivers/goodixd; $(MAKE)
	cd drivers/powerd; $(MAKE)

clean:	
	cd drivers/goodixd; $(MAKE) clean
	cd drivers/powerd; $(MAKE) clean
	rm -fr build
