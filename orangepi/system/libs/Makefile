all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_orangepi;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_orangepi;$(MAKE) clean
	cd bsp;$(MAKE) clean
