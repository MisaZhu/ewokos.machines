ROOT_DIR=..
include $(ROOT_DIR)/../make.inc

BUILD_DIR = $(ROOT_DIR)/build

CFLAGS += -Iinclude

LIB_XGO_OBJS = src/xgo.o

LIB_XGO = libxgo.a

$(LIB_XGO): $(LIB_XGO_OBJS)
	mkdir -p $(BUILD_DIR)/lib
	$(AR) rcs $(BUILD_DIR)/lib/$(LIB_XGO) $(LIB_XGO_OBJS)
	cp -r include $(BUILD_DIR)/

clean:
	rm -f $(LIB_XGO_OBJS)

