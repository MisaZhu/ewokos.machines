all: basic_libs
	@echo "all done."

DIRS = arch_bcm2712 videocore bsp

# videocore and bsp include headers installed by arch_bcm2712 into the SDK
# include dir; bsp_g2d additionally includes videocore/vc_g2d.h
videocore: arch_bcm2712
bsp: arch_bcm2712 videocore

basic_libs: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all basic_libs clean $(DIRS)
