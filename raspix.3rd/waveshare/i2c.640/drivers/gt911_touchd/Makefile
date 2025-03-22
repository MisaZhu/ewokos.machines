TASK_OBJS = gt911.o gt911_touchd.o
TASK = gt911_touchd 

ROOT_DIR=../..
include $(ROOT_DIR)/../../make.inc

BUILD_DIR = $(ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
