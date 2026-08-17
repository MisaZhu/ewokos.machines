EXTRA_LIBS_DIR = ../../../libs

TASK_OBJS = \
		$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o \
		spilcdd.o
TASK = spilcdd 

include ../../../make.inc

CFLAGS += -I $(EXTRA_LIBS_DIR)/st77xx/include

MF_ROOT_DIR=../..
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o: $(EXTRA_LIBS_DIR)/st77xx/src/st77xx.c $(EXTRA_LIBS_DIR)/st77xx/include/st77xx/st77xx.h
	$(CC) $(CFLAGS) -c -o $@ $(EXTRA_LIBS_DIR)/st77xx/src/st77xx.c

$(TASK): $(TASK_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libdisplay.a \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libfont.a \
		$(BUILD_DIR)/lib/libdisplayman.a \
		$(BUILD_DIR)/lib/libdisplayd.a \
		$(BUILD_DIR)/lib/libtinyjson.a \
		$(BUILD_DIR)/lib/libarch_bcm283x.a \
		$(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  -ldisplayd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
