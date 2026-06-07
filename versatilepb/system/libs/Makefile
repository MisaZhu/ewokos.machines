all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_vpb;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_vpb;$(MAKE) clean
	cd bsp;$(MAKE) clean
