MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/make.inc

TASK_OBJS = fbd.o
TASK = fbd 

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd -lupng -lgraph -lbsp $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
