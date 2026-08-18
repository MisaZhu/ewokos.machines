TASK_OBJS = i2cdisplayd.o
TASK = i2cdisplayd

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libdisplayd.a \
		$(SDK_DIR)/lib/libtinyjson.a \
		$(SDK_DIR)/lib/libbsp.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -ldisplayd -ltinyjson $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
