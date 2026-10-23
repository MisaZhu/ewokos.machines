all: builtin_bins
	@echo "machine bins all done."

DIRS = reboot

builtin_bins: $(DIRS)

$(DIRS):
	@$(MAKE) -C $@

.PHONY: all builtin_bins clean $(DIRS)

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
