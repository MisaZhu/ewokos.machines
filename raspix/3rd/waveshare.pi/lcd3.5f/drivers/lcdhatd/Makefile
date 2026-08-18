EXTRA_LIBS_DIR = ../../../../libs

TASK_OBJS = \
		$(EXTRA_LIBS_DIR)/st77xx/src/st77xx.o \
		display.o

TASK = lcd3.5fd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../../make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

CFLAGS += \
	-I $(EXTRA_LIBS_DIR)/st77xx/include

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
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -ldisplayd -ltinyjson $(EWOK_LIB_GRAPH)  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
