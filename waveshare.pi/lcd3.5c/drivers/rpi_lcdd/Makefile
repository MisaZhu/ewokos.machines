MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

EXTRA_LIBS_DIR = ../../../libs

TASK_OBJS = rpi_lcdd.o \
		$(EXTRA_LIBS_DIR)/xpt2046/src/xpt2046.o \
		$(EXTRA_LIBS_DIR)/ili9486/src/ili9486.o

CFLAGS += -I $(EXTRA_LIBS_DIR)/ili9486/include \
		-I $(EXTRA_LIBS_DIR)/xpt2046/include 

TASK = rpi_lcdd 

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd -ltinyjson -lupng -lgraph -lbsp $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)