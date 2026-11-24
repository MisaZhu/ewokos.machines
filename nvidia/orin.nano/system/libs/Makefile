all: ga10b bsp
	@echo "all done."

# ga10b builds before bsp: bsp_g3d forwards into the GA10B driver library
# and includes its installed <ga10b/*.h> headers.
ga10b:
	@$(MAKE) -C ga10b

bsp: ga10b
	@$(MAKE) -C bsp

clean:
	@$(MAKE) -C bsp clean
	@$(MAKE) -C ga10b clean

.PHONY: all bsp ga10b clean
