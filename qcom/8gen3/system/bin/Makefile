# Machine-local user binaries for q8b.  Each subdir has its own Makefile that
# links against the SDK libs staged by the basic_sys phase.
all: nputest pcieprobe reboot
	@echo "bin done."

nputest:
	@$(MAKE) -C nputest

pcieprobe:
	@$(MAKE) -C pcieprobe

reboot:
	@$(MAKE) -C reboot

clean:
	@$(MAKE) -C nputest clean
	@$(MAKE) -C pcieprobe clean
	@$(MAKE) -C reboot clean

.PHONY: all nputest pcieprobe reboot clean
