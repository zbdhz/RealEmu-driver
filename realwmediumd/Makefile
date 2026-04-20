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

# include paths for headers
CFLAGS += -I../include -I../realemu-hw -I../tools

# 主程序目标
TARGET = realwmediumd

# realwmediumd 本身的源文件
SOURCES = realwmediumd.c config.c per.c wserver.c realwmediumd_dynamic.c \
	  wserver_messages.c wserver_messages_network.c

# realwmediumd 本身的对象文件
OBJECTS = $(SOURCES:.c=.o)

# 其他源的库路径
REALEMU_HW_LIB = ../realemu-hw/realemu_hw.o
TOOLS_LIBS = ../tools/reg_rw.o ../tools/dma_with_device.o ../tools/dma_utils.o

# 所有依赖项
ALL_LIBS = $(REALEMU_HW_LIB) $(TOOLS_LIBS)

# 默认目标：构建主程序
all: $(TARGET)

# 构建主程序：先编译依赖库，再编译本地源文件，最后链接
$(TARGET): build-deps $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) -o $@ $(OBJECTS) $(ALL_LIBS) $(LDFLAGS)
	@echo "$(TARGET) built successfully"

# 编译依赖库（调用其他 Makefile）
.PHONY: build-deps
build-deps:
	@echo "Building realemu-hw dependencies..."
	@$(MAKE) -C ../realemu-hw
	@echo "Building tools dependencies..."
	@$(MAKE) -C ../tools

# 本地对象文件编译规则
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "Cleaning realwmediumd..."
	rm -f $(OBJECTS) $(TARGET)
	@echo "Cleaning realwmediumd completed"
	@echo "Cleaning dependencies..."
	@$(MAKE) -C ../realemu-hw clean
	@$(MAKE) -C ../tools clean
	@echo "All clean completed"

.PHONY: all clean build-deps
