# realemu-hw Makefile
# RealEmu硬件接口程序
# 实现与FPGA硬件的交互功能

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

# 添加头文件搜索路径
CFLAGS += -I../include
CFLAGS += -I../tools

# 只需要生成 .o 文件的源文件（不生成可执行文件）
LIB_OBJS = realemu_hw.o

# 所有目标文件
OBJECTS = $(LIB_OBJS)

# 默认只生成库对象文件
all: $(LIB_OBJS)

# 库对象文件编译规则
$(LIB_OBJS): %.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -DBUILD_LIBRARY -c -o $@ $<

# 可选的测试程序（需要显式指定）
test_rtt: realemu_hw.o test_rtt.o
	@echo "Building test_rtt..."
	$(CC) -o $@ realemu_hw.o test_rtt.o $(LDFLAGS)
	@echo "test_rtt built successfully"

test_throughput: realemu_hw.o test_throughput.o
	@echo "Building test_throughput..."
	$(CC) -o $@ realemu_hw.o test_throughput.o $(LDFLAGS)
	@echo "test_throughput built successfully"

test_rtt.o: test_rtt.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

test_throughput.o: test_throughput.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "Cleaning realemu-hw..."
	rm -f $(OBJECTS) test_rtt test_throughput test_rtt.o test_throughput.o
	@echo "Clean completed"
