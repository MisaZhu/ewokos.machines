ROOT_DIR=../..
include $(ROOT_DIR)/../make.inc

TASK_OBJS = soundd.o i2s.o wm8960.o dma_chain.o 
TASK = soundd

BUILD_DIR = $(ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
