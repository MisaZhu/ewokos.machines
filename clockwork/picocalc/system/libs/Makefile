all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_rk3506;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_rk3506;$(MAKE) clean
	cd bsp;$(MAKE) clean
