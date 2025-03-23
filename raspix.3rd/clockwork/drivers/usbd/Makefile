EWOKOS_ROOT_DIR = ../../../../..
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

LDFLAGS += -Llib 
CFLAGS += -I include

TASK_OBJS = usbd.o
TASK = usbd 

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC) -lcsud -lewoksys

clean:
	rm -f $(TASK_OBJS)