TASK_OBJS = fbd.o lcdhatd.o
TASK = lcdhatd 

ROOT_DIR=../..
include $(ROOT_DIR)/../../make.inc

BUILD_DIR = $(ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd -ltinyjson -lupng -lgraph -lbsp $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
