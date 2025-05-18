MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/make.inc

LDFLAGS += -Llib 
CFLAGS += -I include

TASK_OBJS = usbd.o
TASK = usbd 

BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS) \
		$(EWOK_LIBC_A)  \
		lib/libcsud_$(ARCH).a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) -lcsud_$(ARCH) $(EWOK_LIBC) -lewoksys

clean:
	rm -f $(TASK_OBJS)