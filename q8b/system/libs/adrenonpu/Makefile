include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# Include paths (make.rule re-assigns CFLAGS, discarding make.inc's -I list,
# so every path this library needs is re-appended here):
#   -I include                         : adrenonpu/*.h (this lib's public headers)
#   -I $(MAKEINC_DIR)/../include       : q8b_adreno.h / q8b_gpu_pas.h (machine shared)
#   -I $(TARGET_DIR)/include           : installed platform headers
#   -I libewoksys/.../include          : ewoksys/vfs.h, ewoksys/dma.h, ...
# The machine libs phase runs before system/basic, so ewoksys headers must also
# be available directly from the source tree (same rule as the adreno lib).
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(MAKEINC_DIR)/../include \
	-I include -I $(TARGET_DIR)/include

# QNN C API core (matches the qnn_core.h / qnn_htp_ext.h contract).
LIB_ADRENONPU_OBJS = src/qnn_log.o \
	src/qnn_handle.o \
	src/qnn_ops.o \
	src/qnn_interface.o \
	src/qnn_backend.o \
	src/qnn_device.o \
	src/qnn_context.o \
	src/qnn_tensor.o \
	src/qnn_graph.o \
	src/qnn_mem.o \
	src/qnn_signal.o \
	src/qnn_profile.o \
	src/qnn_htp_ext.o \
	src/npu_fastrpc.o \
	src/npu_cdsp.o \
	src/npu_htp.o \
	src/npu_ref_exec.o

ADRENONPU = $(TARGET_DIR)/lib/libadrenonpu.a

$(ADRENONPU): $(LIB_ADRENONPU_OBJS)
	$(AR) rcs $(ADRENONPU) $(LIB_ADRENONPU_OBJS)
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

# Pull in the generated .d files so a header-only edit triggers a rebuild
# (the shared make.rule passes -MMD -MP but never includes them).
-include $(LIB_ADRENONPU_OBJS:.o=.d)

clean:
	rm -f $(LIB_ADRENONPU_OBJS)
	rm -f $(LIB_ADRENONPU_OBJS:.o=.d)
	rm -f $(ADRENONPU)

.PHONY: clean
