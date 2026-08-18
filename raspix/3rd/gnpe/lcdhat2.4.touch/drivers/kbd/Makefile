TASK_OBJS = kbd.o
TASK = kbd 

include ../../../../make.inc

MF_ROOT_DIR=../..
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libarch_bcm283x.a \
		$(SDK_DIR)/lib/libbsp.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
