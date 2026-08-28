export MF=xgo

DIRS = lib drivers/spilcdd drivers/buttond drivers/soundd drivers/micd apps/xgo apps/cam apps/xgoAI
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
