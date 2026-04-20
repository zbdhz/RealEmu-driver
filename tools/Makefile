# tools Makefile
# 与xdma驱动抽象出来的文件相交互
# 实现上层数据和fpga数据的交换
# 主要包含xdma转换后的axi-stream的上下行接口操作和axi-lite的寄存器接口操作
VERSION_STR="\"1.0\""

# CC = gcc
# Look for libnl libraries
PKG_CONFIG ?= pkg-config

NL2FOUND := $(shell $(PKG_CONFIG) --atleast-version=2 libnl-2.0 && echo Y)
NL3FOUND := $(shell $(PKG_CONFIG) --atleast-version=3 libnl-3.0 && echo Y)
NL31FOUND := $(shell $(PKG_CONFIG) --exact-version=3.1 libnl-3.1 && echo Y)
NL3xFOUND := $(shell $(PKG_CONFIG) --atleast-version=3.2 libnl-3.0 && echo Y)

CFLAGS = -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2
LDFLAGS = -levent -lm

ifeq ($(NL2FOUND),Y)
CFLAGS += -DCONFIG_LIBNL20
LDFLAGS += -lnl-genl
NLLIBNAME = libnl-2.0
endif

ifeq ($(NL3xFOUND),Y)
# libnl 3.2 might be found as 3.2 and 3.0
NL3FOUND = N
CFLAGS += -DCONFIG_LIBNL30
LDFLAGS += -lnl-genl-3
NLLIBNAME = libnl-3.0
endif

ifeq ($(NL3FOUND),Y)
CFLAGS += -DCONFIG_LIBNL30
LDFLAGS += -lnl-genl
NLLIBNAME = libnl-3.0
endif

# nl-3.1 has a broken libnl-gnl-3.1.pc file
# as show by pkg-config --debug --libs --cflags --exact-version=3.1 libnl-genl-3.1;echo $?
ifeq ($(NL31FOUND),Y)
CFLAGS += -DCONFIG_LIBNL30
LDFLAGS += -lnl-genl
NLLIBNAME = libnl-3.1
endif

ifeq ($(NLLIBNAME),)
$(error Cannot find development files for any supported version of libnl)
endif

LDFLAGS += $(shell $(PKG_CONFIG) --libs $(NLLIBNAME))
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(NLLIBNAME))

CFLAGS += -DVERSION_STR=$(VERSION_STR)
LDFLAGS += -lconfig -lpthread

# 只需要生成 .o 文件的源文件（不生成可执行文件）
LIB_OBJS = reg_rw.o dma_with_device.o dma_utils.o

# 所有目标文件
OBJECTS = $(LIB_OBJS)

# 默认只生成库对象文件
all: $(LIB_OBJS)

# 库文件编译规则
$(LIB_OBJS): %.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -DBUILD_LIBRARY -c -o $@ $<

# 可选的测试程序（需要显式指定）
dma_test: dma_test.o $(LIB_OBJS)
	@echo "Building dma_test..."
	$(CC) -o $@ dma_test.o $(LIB_OBJS) $(LDFLAGS)
	@echo "dma_test built successfully"

cfg_test: cfg_test.o reg_rw.o dma_utils.o
	@echo "Building cfg_test..."
	$(CC) -o $@ cfg_test.o reg_rw.o dma_utils.o $(LDFLAGS)
	@echo "cfg_test built successfully"

dma_test.o: dma_test.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

cfg_test.o: cfg_test.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "Cleaning tools..."
	rm -f $(OBJECTS) dma_test cfg_test dma_test.o cfg_test.o
	@echo "Clean completed"