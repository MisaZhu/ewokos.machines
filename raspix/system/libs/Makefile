all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_bcm283x;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_bcm283x;$(MAKE) clean
	cd bsp; $(MAKE) clean
