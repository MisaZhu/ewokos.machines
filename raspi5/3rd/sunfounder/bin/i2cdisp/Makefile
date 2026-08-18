TASK_OBJS = i2cdisp.o
TASK = i2cdisp

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/bin

all: $(TASK)

$(TASK): $(TASK_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) $(EWOK_LIB_GRAPH) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
