export MF=xgo

DIRS = lib drivers/spilcdd drivers/buttond drivers/soundd drivers/micd apps/xgo apps/cam apps/xgoAI
all: 
	mkdir -p build 
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc build 

CLEAN_DIRS = drivers/spilcdd drivers/buttond drivers/soundd drivers/micd lib apps/xgo apps/cam apps/xgoAI
clean:	
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
