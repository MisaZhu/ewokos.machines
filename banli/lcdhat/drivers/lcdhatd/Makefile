EXTRA_LIBS_DIR = ../../../libs
CFLAGS += -I $(EXTRA_LIBS_DIR)/xpt2046/include

TASK_OBJS = fbd.o lcdhatd.o \
		$(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.o
TASK = lcdhatd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd -ltinyjson $(EWOK_LIB_GRAPH) -lbsp $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
