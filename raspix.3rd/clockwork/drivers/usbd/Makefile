ROOT_DIR=../..
include $(ROOT_DIR)/../make.inc

LDFLAGS += -Llib 
CFLAGS += -I include

TASK_OBJS = usbd.o
TASK = usbd 

BUILD_DIR = $(ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC) -lcsud -lewoksys

clean:
	rm -f $(TASK_OBJS)