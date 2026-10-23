# Machine-local user binaries for q8b.  Each subdir has its own Makefile that
# links against the SDK libs staged by the basic_sys phase.
all: nputest
	@echo "bin done."

nputest:
	@$(MAKE) -C nputest

clean:
	@$(MAKE) -C nputest clean

.PHONY: all nputest clean
