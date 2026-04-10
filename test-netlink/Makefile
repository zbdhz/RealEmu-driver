# 内核模块编译
obj-m += netlink_test.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC := gcc-12

# 构建目录
BUILD_DIR := build
USER_PROG := $(BUILD_DIR)/netlink_user
KERNEL_MOD := $(BUILD_DIR)/netlink_test.ko

# 编译目标
all: $(BUILD_DIR) kernel_modules user_program

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

kernel_modules: $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(PWD) CC=$(CC) modules
	mv *.o *.ko *.mod* .*.cmd Module.symvers modules.order $(BUILD_DIR)/ 2>/dev/null || true
	mv .tmp_versions $(BUILD_DIR)/ 2>/dev/null || true

user_program: $(USER_PROG)

$(USER_PROG): netlink_user.c | $(BUILD_DIR)
	gcc -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf $(BUILD_DIR)
	rm -f *.o *.ko *.mod* .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

# 加载/卸载内核模块（需要root权限）
load: $(KERNEL_MOD)
	sudo insmod $(KERNEL_MOD)

unload:
	sudo rmmod netlink_test

# 运行用户空间程序
run: user_program
	./$(USER_PROG)

.PHONY: all clean kernel_modules user_program load unload run
