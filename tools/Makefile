# tools Makefile
# 与xdma驱动抽象出来的文件相交互
# 实现上层数据和fpga数据的交换
# 主要包含xdma转换后的axi-stream的上下行接口操作和axi-lite的寄存器接口操作

CC = gcc
CFLAGS = -Wall -O2 -I../include
LDFLAGS = 

# 静态库的源文件（不包含main函数）
SRCS = dma_from_device.c dma_to_device.c reg_rw.c
OBJS = $(SRCS:.c=.o)
LIB = libtools.a

# 独立程序的源文件（包含main函数）
PROGS = reg_rw dma_to_device dma_from_device

all: $(LIB) $(PROGS)

# 编译静态库
$(LIB): $(LIB_OBJS)
	@echo "Building tools library..."
	$(AR) rcs $@ $(LIB_OBJS)
	@echo "Tools library built: $(LIB)"

# 编译独立程序
reg_rw: reg_rw.c
	@echo "Building reg_rw..."
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "reg_rw built"

dma_to_device: dma_to_device.c dma_utils.c
	@echo "Building dma_to_device..."
	$(CC) $(CFLAGS) -o $@ dma_to_device.c $(LDFLAGS)
	@echo "dma_to_device built"

dma_from_device: dma_from_device.c dma_utils.c
	@echo "Building dma_from_device..."
	$(CC) $(CFLAGS) -o $@ dma_from_device.c $(LDFLAGS)
	@echo "dma_from_device built"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning tools..."
	rm -f $(LIB_OBJS) $(LIB) $(PROGS)
	@echo "Tools cleaned"