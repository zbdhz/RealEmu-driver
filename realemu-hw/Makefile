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

# 目标可执行文件
TARGET = realemu_hw

# 主程序源文件
MAIN_SRC = realemu_hw.c

# 主程序目标文件
MAIN_OBJ = $(MAIN_SRC:.c=.o)

# 依赖的 tools 目录中的目标文件
TOOLS_OBJS = ../tools/reg_rw.o ../tools/dma_with_device.o ../tools/dma_utils.o

# 所有目标文件
OBJECTS = $(MAIN_OBJ)

all: $(TARGET)

$(TARGET): $(MAIN_OBJ) $(TOOLS_OBJS)
	@echo "Building $(TARGET)..."
	$(CC) -o $@ $(MAIN_OBJ) $(TOOLS_OBJS) $(LDFLAGS)
	@echo "$(TARGET) built successfully"

# 编译主程序
$(MAIN_OBJ): $(MAIN_SRC)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "Cleaning $(MAIN_OBJ) $(TARGET)..."
	rm -f $(MAIN_OBJ) $(TARGET)
	@echo "Clean completed"
