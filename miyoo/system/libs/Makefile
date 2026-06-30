all: basic_libs
	@echo "all done."

basic_libs:
	cd arch_miyoo;$(MAKE)
	cd bsp;$(MAKE)

clean:
	cd arch_miyoo;$(MAKE) clean
	cd bsp;$(MAKE) clean
