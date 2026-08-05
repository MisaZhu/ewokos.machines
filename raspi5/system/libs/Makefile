all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_bcm2712;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_bcm2712;$(MAKE) clean
	cd bsp; $(MAKE) clean
