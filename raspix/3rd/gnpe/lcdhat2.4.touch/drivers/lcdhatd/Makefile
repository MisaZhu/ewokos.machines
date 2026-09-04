EXTRA_LIBS_DIR = ../../../../libs

TASK_OBJS = \
		$(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.o \
		$(EXTRA_LIBS_DIR)/xpt2046/src/tp_filter.o \
		$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o \
		display.o

TASK = lcd2.4touchd 

include ../../../../make.inc

CFLAGS += -I $(EXTRA_LIBS_DIR)/xpt2046/include \
	-I $(EXTRA_LIBS_DIR)/st77xx/include

MF_ROOT_DIR=../..
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o: $(EXTRA_LIBS_DIR)/st77xx/src/st77xx.c $(EXTRA_LIBS_DIR)/st77xx/include/st77xx/st77xx.h
	$(CC) $(CFLAGS) -c -o $@ $(EXTRA_LIBS_DIR)/st77xx/src/st77xx.c

$(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.o: $(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.c $(EXTRA_LIBS_DIR)/xpt2046/include/xpt2046/xpt2046.h
	$(CC) $(CFLAGS) -c -o $@ $(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.c

$(EXTRA_LIBS_DIR)/xpt2046/src/tp_filter.o: $(EXTRA_LIBS_DIR)/xpt2046/src/tp_filter.c $(EXTRA_LIBS_DIR)/xpt2046/include/xpt2046/tp_filter.h
	$(CC) $(CFLAGS) -c -o $@ $(EXTRA_LIBS_DIR)/xpt2046/src/tp_filter.c

$(TASK): $(TASK_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libdisplayd.a \
		$(SDK_DIR)/lib/libtinyjson.a \
		$(SDK_DIR)/lib/libarch_bcm283x.a \
		$(SDK_DIR)/lib/libbsp.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -ldisplayd -ltinyjson $(EWOK_LIB_GRAPH)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
