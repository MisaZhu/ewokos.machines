EXTRA_LIBS_DIR = ../../../../libs
TASK_OBJS = \
		$(EXTRA_LIBS_DIR)/gt911/src/gt911.o \
		gt911_touchd.o

TASK = gt911_touchd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

CFLAGS += -I $(EXTRA_LIBS_DIR)/gt911/include

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
