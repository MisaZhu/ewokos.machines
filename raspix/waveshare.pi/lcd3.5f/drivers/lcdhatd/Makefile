EXTRA_LIBS_DIR = ../../../../libs

TASK_OBJS = \
		$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o \
		fbd.o

TASK = lcdhatd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

CFLAGS += \
	-I $(EXTRA_LIBS_DIR)/st77xx/include

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd -ltinyjson $(EWOK_LIB_GRAPH)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
