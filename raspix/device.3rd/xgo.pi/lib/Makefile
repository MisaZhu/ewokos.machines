EWOKOS_ROOT_DIR = ../../../..
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build

MF_ROOT_DIR=..
include $(MF_ROOT_DIR)/make.inc

BUILD_DIR = $(MF_ROOT_DIR)/build

CFLAGS += -Iinclude

LIB_XGO_OBJS = src/xgo.o

LIB_XGO = libxgo.a

$(LIB_XGO): $(LIB_XGO_OBJS)
	mkdir -p $(BUILD_DIR)/lib
	$(AR) rcs $(BUILD_DIR)/lib/$(LIB_XGO) $(LIB_XGO_OBJS)
	cp -r include $(BUILD_DIR)/

clean:
	rm -f $(LIB_XGO_OBJS)

