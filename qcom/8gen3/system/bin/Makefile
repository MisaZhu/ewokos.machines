# Machine-local user binaries for qcom8gen3.  Each subdir has its own Makefile that
# links against the SDK libs staged by the basic_sys phase.
all: nputest pcieprobe reboot shutdown adspas
	@echo "bin done."

nputest:
	@$(MAKE) -C nputest

adspas:
	@$(MAKE) -C adspas

pcieprobe:
	@$(MAKE) -C pcieprobe

reboot:
	@$(MAKE) -C reboot

shutdown:
	@$(MAKE) -C shutdown

clean:
	@$(MAKE) -C nputest clean
	@$(MAKE) -C pcieprobe clean
	@$(MAKE) -C reboot clean
	@$(MAKE) -C shutdown clean
	@$(MAKE) -C adspas clean

.PHONY: all nputest pcieprobe reboot shutdown adspas clean
