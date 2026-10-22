include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule
BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)
CFLAGS += -I $(BUILD_DIR)/include
LDFLAGS = -L $(BUILD_DIR)/lib
OBJS = $(ARCH)/gpusmoke.o
TARGET = $(TARGET_DIR)/bin/gpusmoke
SCANOUT = $(TARGET_DIR)/bin/g2dscanout
PRESENT = $(TARGET_DIR)/bin/g2dpresent
all: $(TARGET) $(SCANOUT) $(PRESENT)
$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libgraph.a $(BUILD_DIR)/lib/libg2dclient.a $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) -lgraph -lg2dclient -lopenlibm $(EWOK_LIBC)
$(SCANOUT): $(ARCH)/g2dscanout.o $(BUILD_DIR)/lib/libdisplayd.a $(BUILD_DIR)/lib/libgraph.a $(BUILD_DIR)/lib/libg2dclient.a $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(ARCH)/g2dscanout.o -o $(SCANOUT) $(LDFLAGS) -ldisplayd $(EWOK_LIB_GRAPH) -ltinyjson $(EWOK_LIBC)
$(PRESENT): $(ARCH)/g2dpresent.o $(BUILD_DIR)/lib/libdisplay.a $(BUILD_DIR)/lib/libgraph.a $(BUILD_DIR)/lib/libg2dclient.a $(BUILD_DIR)/lib/libbsp.a $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(ARCH)/g2dpresent.o -o $(PRESENT) $(LDFLAGS) $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) $(EWOK_LIBC)
clean:
	rm -f $(OBJS) $(ARCH)/g2dscanout.o $(ARCH)/g2dpresent.o $(TARGET) $(SCANOUT) $(PRESENT)
.PHONY: all clean
