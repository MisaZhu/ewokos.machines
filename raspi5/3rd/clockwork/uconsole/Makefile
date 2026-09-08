export MF=clockwork

#
# The two display daemons that used to be built here are gone: fbdisplayd
# (original cwu50 batch + cwd686) and fbdisplay6d (newer cwu50 batch,
# plus the contrast knob) are now panel families inside
# system/drivers/dsi_fbdisplayd, picked by /etc/display.json's "panel"
# key -- cwu50 / cwu50_old / cwd686 -- and launched from init.rd as
# /drivers/raspi5/dsi_fbdisplayd.  drivers/dsi/ is kept because it is the
# provenance source those families were transcribed from; cwu50_old and
# cwd686 are not hardware-verified yet, so the reference still matters.
#
DIRS = drivers/powerd drivers/soundpwmd
all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build/

clean:	
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
