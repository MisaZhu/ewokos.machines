# sherpa-onnx minimal port for EwokOS (aarch64 / arm).
#
# Builds libsherpa-onnx.a and installs it, together with the public headers,
# into the EwokOS SDK dir ($(ewokos)/system/build/$(HW)).
#
# Usage:
#   make            # ARCH=aarch64 (default), HW=raspix
#   make ARCH=arm   # 32-bit arm
#   make clean

include ../../make.inc

BUILD_DIR = $(SYS_BUILD_DIR)/$(HW)

CXXFLAGS += -I include -I $(BUILD_DIR)/include

LIB_OBJS = src/fbank.o src/onnx.o src/sherpa.o
TARGET = $(BUILD_DIR)/lib/libsherpa-onnx.a

all: $(TARGET)

$(TARGET): $(LIB_OBJS)
	mkdir -p $(BUILD_DIR)/lib
	mkdir -p $(BUILD_DIR)/include/sherpa-onnx
	$(AR) rs $(TARGET) $(LIB_OBJS)
	cp include/*.h $(BUILD_DIR)/include/sherpa-onnx/

src/%.o: src/%.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(LIB_OBJS)
	rm -f $(TARGET)
