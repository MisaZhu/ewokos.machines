# Optional native Hexagon runtime. QAIRT remains an external dependency.
.DEFAULT_GOAL := all
HEXAGON_CLANG ?= clang
HEXAGON_LD ?= ld.lld
PYTHON ?= python3
OUT ?= build
QNN_INCLUDE = $(QAIRT_SDK_ROOT)/include/QNN
QNN_LIB = $(QAIRT_SDK_ROOT)/lib/hexagon-v68/unsigned
CFLAGS = --target=hexagon -mcpu=hexagonv68 -fPIC -ffreestanding \
	-fno-short-enums -O2 -Wall -Wextra -Werror \
	-I"$(QNN_INCLUDE)" -I../include
OBJECTS = $(OUT)/runtime.o $(OUT)/model.o

all: $(OUT)/libq8b_npu.so

check_sdk:
	@test -n "$(QAIRT_SDK_ROOT)" && test -f "$(QNN_INCLUDE)/QnnInterface.h" && \
		test -f "$(QNN_LIB)/libQnnHtpV68.so" && test -f "$(QNN_LIB)/libQnnSystem.so" || \
		{ echo "Set QAIRT_SDK_ROOT to a versioned QAIRT SDK with V68 libraries"; exit 1; }

$(OUT)/%.o: %.c rpc.h $(wildcard ../include/adrenonpu/npu_*.h) | check_sdk
	@mkdir -p "$(OUT)"
	$(HEXAGON_CLANG) $(CFLAGS) -MMD -MP -c $< -o "$@"

$(OUT)/libq8b_npu.so: $(OBJECTS) linker.ld tag_abi.py \
		$(QNN_LIB)/libQnnHtpV68.so $(QNN_LIB)/libQnnSystem.so
	$(HEXAGON_LD) -shared --hash-style=sysv -z norelro -z max-page-size=4096 \
		-T linker.ld -init=q8b_dsp_init -fini=q8b_dsp_fini -soname libq8b_npu.so \
		$(OBJECTS) "$(QNN_LIB)/libQnnHtpV68.so" -L"$(QNN_LIB)" -l:libQnnSystem.so -o "$@.tmp"
	$(PYTHON) tag_abi.py "$@.tmp"
	mv "$@.tmp" "$@"

-include $(OBJECTS:.o=.d)
.PHONY: all check_sdk
