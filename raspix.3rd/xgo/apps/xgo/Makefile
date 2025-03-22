ROOT_DIR=../..
include $(ROOT_DIR)/../make.inc

TASK_OBJS = xgo.o

TASK = xgo
BUILD_DIR = $(ROOT_DIR)/build/apps/$(TASK)

CXXFLAGS += -I $(ROOT_DIR)/build/include
LDFLAGS += -L$(ROOT_DIR)/build/lib

all: $(TASK)

$(TASK): $(TASK_OBJS) 
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lxgo $(EWOK_LIB_X) $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
	cp -r res $(BUILD_DIR)

clean:
	rm -f $(TASK_OBJS)


