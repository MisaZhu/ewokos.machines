TASK_OBJS = micd.o wm8960_shared.o
TASK = micd

include ../../../make.inc

MF_ROOT_DIR=../..
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

wm8960_shared.o: ../soundd/wm8960.c ../soundd/wm8960.h
	$(CC) $(CFLAGS) -c ../soundd/wm8960.c -o $@

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
