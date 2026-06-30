all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_ev3;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_ev3;$(MAKE) clean
	cd bsp;$(MAKE) clean
