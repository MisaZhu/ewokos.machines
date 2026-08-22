export HW=raspix
export MF=gnpe

DIRS = drivers/kbd drivers/lcdhatd drivers/soundpwmd

all: 
	mkdir -p build
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build

clean:	
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr build
