/*
 * AXI-Lite配置接口测试程序
 * 用于测试RealEmu项目中寄存器配置功能的正确性
 * 通过调用reg_rw中的函数实现寄存器读写测试
 * 测试节点0的mac_retry_limit寄存器（偏移地址0x30）
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "reg_rw.h"

/* 测试配置 */
#define DEVICE_LITE "/dev/xdma0_user"  // AXI-Lite设备文件

/* RealEmu节点配置 */
#define REALEMU_NODE_BASE_ADDR    0x00100000
#define REALEMU_NODE_SIZE        0x00000400
#define REALEMU_MAC_OFFSET       0x00000000

/* 测试寄存器配置 */
#define TEST_NODE_ID            0           // 测试节点ID
#define MAC_RETRY_LIMIT_OFFSET   0x30        // mac_retry_limit寄存器偏移

/* 计算测试地址 */
#define TEST_MAC_BASE_ADDR      (REALEMU_NODE_BASE_ADDR + (TEST_NODE_ID * REALEMU_NODE_SIZE) + REALEMU_MAC_OFFSET)
#define TEST_REG_ADDR          (TEST_MAC_BASE_ADDR + MAC_RETRY_LIMIT_OFFSET)

/* 测试统计 */
typedef struct {
    uint32_t read_count;      // 读取次数
    uint32_t write_count;     // 写入次数
    uint32_t error_count;     // 错误次数
    uint32_t success_count;   // 成功次数
    struct timeval start_time;
    struct timeval end_time;
} TestStats;

/*
 * 打印测试统计信息
 */
void print_stats(const TestStats *stats) {
    double duration_sec;
    
    duration_sec = (stats->end_time.tv_sec - stats->start_time.tv_sec) +
                   (stats->end_time.tv_usec - stats->start_time.tv_usec) / 1000000.0;
    
    printf("\n========== AXI-Lite配置测试统计 ==========\n");
    printf("测试时长: %.3f 秒\n", duration_sec);
    printf("读取次数: %u\n", stats->read_count);
    printf("写入次数: %u\n", stats->write_count);
    printf("成功次数: %u\n", stats->success_count);
    printf("错误次数: %u\n", stats->error_count);
    printf("======================================\n");
}

/*
 * 测试1: 基本寄存器读写测试
 * 测试mac_retry_limit寄存器的基本读写功能
 */
int test_basic_reg_rw(int fd) {
    uint32_t test_addr = TEST_REG_ADDR;
    uint32_t write_value = 0x0000000A;  // 默认重试次数为10
    uint32_t read_value = 0;
    int ret;
    int result = 0;
    
    printf("\n========== 测试1: 基本寄存器读写测试 ==========\n");
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("寄存器地址: 0x%08X\n", test_addr);
    
    // 读取原始值
    ret = reg_read(fd, test_addr, &read_value);
    if (ret != 0) {
        fprintf(stderr, "读取寄存器失败\n");
        return -1;
    }
    printf("原始值: 0x%08X (重试次数: %u)\n", read_value, read_value);
    
    // 写入测试值
    write_value = 0x0000000F;  // 设置重试次数为15
    ret = reg_write(fd, test_addr, write_value);
    if (ret != 0) {
        fprintf(stderr, "写入寄存器失败\n");
        return -1;
    }
    printf("写入值: 0x%08X (重试次数: %u)\n", write_value, write_value);
    
    // 读取并验证
    ret = reg_read(fd, test_addr, &read_value);
    if (ret != 0) {
        fprintf(stderr, "读取寄存器失败\n");
        return -1;
    }
    printf("读取值: 0x%08X (重试次数: %u)\n", read_value, read_value);
    
    // 验证读写一致性
    if (read_value == write_value) {
        printf("读写一致性验证成功！\n");
        result = 0;
    } else {
        printf("读写一致性验证失败！期望: 0x%08X, 实际: 0x%08X\n", 
               write_value, read_value);
        result = -1;
    }
    
    printf("基本寄存器读写测试 %s\n", (result == 0) ? "通过" : "失败");
    return result;
}

/*
 * 测试2: 多个重试值测试
 * 测试不同的重试次数值（RetryTime为4位，范围0-15）
 */
int test_retry_values(int fd) {
    uint32_t test_addr = TEST_REG_ADDR;
    uint32_t retry_values[] = {
        0x00000000,  // 重试次数为0
        0x00000001,  // 重试次数为1
        0x00000005,  // 重试次数为5
        0x0000000A,  // 重试次数为10
        0x0000000F   // 重试次数为15（最大值）
    };
    uint32_t read_value;
    int num_tests = sizeof(retry_values) / sizeof(retry_values[0]);
    int success_count = 0;
    int result = 0;
    
    printf("\n========== 测试2: 多个重试值测试 ==========\n");
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("测试值数量: %d\n", num_tests);
    
    for (int i = 0; i < num_tests; i++) {
        // 写入重试值
        int ret = reg_write(fd, test_addr, retry_values[i]);
        if (ret != 0) {
            fprintf(stderr, "写入重试值 0x%08X 失败\n", retry_values[i]);
            continue;
        }
        
        // 读取并验证
        ret = reg_read(fd, test_addr, &read_value);
        if (ret != 0) {
            fprintf(stderr, "读取重试值 0x%08X 失败\n", retry_values[i]);
            continue;
        }
        
        if (read_value == retry_values[i]) {
            printf("重试值 0x%08X (%u次): 读写一致 ✓\n", 
                   retry_values[i], retry_values[i]);
            success_count++;
        } else {
            printf("重试值 0x%08X (%u次): 写入=0x%08X, 读取=0x%08X ✗\n", 
                   retry_values[i], retry_values[i], retry_values[i], read_value);
        }
    }
    
    printf("成功: %d/%d\n", success_count, num_tests);
    
    if (success_count == num_tests) {
        printf("多个重试值测试通过！\n");
        result = 0;
    } else {
        printf("多个重试值测试失败！\n");
        result = -1;
    }
    
    return result;
}

/*
 * 测试3: 边界值测试
 * 测试重试次数的边界值（RetryTime为4位，范围0-15）
 */
int test_boundary_values(int fd) {
    uint32_t test_addr = TEST_REG_ADDR;
    uint32_t boundary_values[] = {
        0x00000000,  // 最小重试次数
        0x00000001,  // 最小非零重试次数
        0x0000000F,  // 最大重试次数（4位最大值）
        0x00000008,  // 中间值
        0x0000000A   // 常用值
    };
    uint32_t read_value;
    int num_tests = sizeof(boundary_values) / sizeof(boundary_values[0]);
    int success_count = 0;
    int result = 0;
    
    printf("\n========== 测试3: 边界值测试 ==========\n");
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("测试值数量: %d\n", num_tests);
    
    for (int i = 0; i < num_tests; i++) {
        // 写入边界值
        int ret = reg_write(fd, test_addr, boundary_values[i]);
        if (ret != 0) {
            fprintf(stderr, "写入边界值 0x%08X 失败\n", boundary_values[i]);
            continue;
        }
        
        // 读取并验证
        ret = reg_read(fd, test_addr, &read_value);
        if (ret != 0) {
            fprintf(stderr, "读取边界值 0x%08X 失败\n", boundary_values[i]);
            continue;
        }
        
        if (read_value == boundary_values[i]) {
            printf("边界值 0x%08X (%u次): 读写一致 ✓\n", 
                   boundary_values[i], boundary_values[i]);
            success_count++;
        } else {
            printf("边界值 0x%08X (%u次): 写入=0x%08X, 读取=0x%08X ✗\n", 
                   boundary_values[i], boundary_values[i], boundary_values[i], read_value);
        }
    }
    
    printf("成功: %d/%d\n", success_count, num_tests);
    
    if (success_count == num_tests) {
        printf("边界值测试通过！\n");
        result = 0;
    } else {
        printf("边界值测试失败！\n");
        result = -1;
    }
    
    return result;
}

/*
 * 测试4: 连续读写性能测试
 * 测试多次连续读写的性能（RetryTime为4位，范围0-15）
 */
int test_continuous_rw(int fd) {
    uint32_t test_addr = TEST_REG_ADDR;
    uint32_t write_value, read_value;
    TestStats stats = {0};
    int num_iterations = 1000;
    int result = 0;
    
    printf("\n========== 测试4: 连续读写性能测试 ==========\n");
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("迭代次数: %d\n", num_iterations);
    
    gettimeofday(&stats.start_time, NULL);
    
    for (int i = 0; i < num_iterations; i++) {
        write_value = (uint32_t)(i & 0x0F);  // 限制在0-15范围内（4位）
        
        // 写入
        int ret = reg_write(fd, test_addr, write_value);
        if (ret != 0) {
            fprintf(stderr, "写入失败 (迭代 %d)\n", i);
            stats.error_count++;
            continue;
        }
        stats.write_count++;
        
        // 读取
        ret = reg_read(fd, test_addr, &read_value);
        if (ret != 0) {
            fprintf(stderr, "读取失败 (迭代 %d)\n", i);
            stats.error_count++;
            continue;
        }
        stats.read_count++;
        
        // 验证
        if (read_value == write_value) {
            stats.success_count++;
        } else {
            stats.error_count++;
        }
        
        // 每100次打印一次进度
        if ((i + 1) % 100 == 0) {
            printf("已完成 %d/%d 次迭代\n", i + 1, num_iterations);
        }
    }
    
    gettimeofday(&stats.end_time, NULL);
    
    // 打印统计
    print_stats(&stats);
    
    // 判断测试结果
    if (stats.error_count == 0) {
        printf("连续读写性能测试通过！\n");
        result = 0;
    } else {
        printf("连续读写性能测试失败！\n");
        result = -1;
    }
    
    return result;
}

/*
 * 测试5: 寄存器回环测试
 * 测试写入的值能够被正确读回（RetryTime为4位，范围0-15）
 */
int test_register_loopback(int fd) {
    uint32_t test_addr = TEST_REG_ADDR;
    uint32_t write_value, read_value;
    int num_tests = 16;  // 测试0-15的所有值（4位范围）
    int success_count = 0;
    int result = 0;
    
    printf("\n========== 测试5: 寄存器回环测试 ==========\n");
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("测试值数量: %d (0-15，4位范围)\n", num_tests);
    
    for (int i = 0; i < num_tests; i++) {
        write_value = (uint32_t)i;
        
        // 写入
        int ret = reg_write(fd, test_addr, write_value);
        if (ret != 0) {
            fprintf(stderr, "写入失败 (值 0x%08X)\n", write_value);
            continue;
        }
        
        // 读取
        ret = reg_read(fd, test_addr, &read_value);
        if (ret != 0) {
            fprintf(stderr, "读取失败 (值 0x%08X)\n", write_value);
            continue;
        }
        
        // 验证
        if (read_value == write_value) {
            success_count++;
        } else {
            printf("回环失败: 写入=0x%08X (%u), 读取=0x%08X (%u)\n", 
                   write_value, write_value, read_value, read_value);
        }
    }
    
    printf("成功: %d/%d\n", success_count, num_tests);
    
    if (success_count == num_tests) {
        printf("寄存器回环测试通过！\n");
        result = 0;
    } else {
        printf("寄存器回环测试失败！\n");
        result = -1;
    }
    
    return result;
}

/*
 * 主函数
 */
int main(int argc, char *argv[]) {
    int fd;
    int ret;
    int test_results[5] = {0};
    
    printf("========================================\n");
    printf("    AXI-Lite配置接口测试程序\n");
    printf("========================================\n");
    printf("设备: %s\n", DEVICE_LITE);
    printf("测试节点: %d\n", TEST_NODE_ID);
    printf("测试寄存器: mac_retry_limit\n");
    printf("寄存器地址: 0x%08X\n", TEST_REG_ADDR);
    printf("========================================\n");
    
    // 打开设备文件
    fd = open(DEVICE_LITE, O_RDWR);
    if (fd < 0) {
        perror("Failed to open AXI-Lite device");
        fprintf(stderr, "请确保设备文件存在且有访问权限\n");
        fprintf(stderr, "可能需要使用 sudo 运行此程序\n");
        return -1;
    }
    
    printf("设备打开成功 (fd=%d)\n", fd);
    
    // 运行测试1: 基本寄存器读写
    test_results[0] = test_basic_reg_rw(fd);
    
    // 运行测试2: 多个重试值测试
    test_results[1] = test_retry_values(fd);
    
    // 运行测试3: 边界值测试
    test_results[2] = test_boundary_values(fd);
    
    // 运行测试4: 连续读写性能测试
    test_results[3] = test_continuous_rw(fd);
    
    // 运行测试5: 寄存器回环测试
    test_results[4] = test_register_loopback(fd);
    
    // 关闭设备文件
    close(fd);
    
    // 打印测试汇总
    printf("\n========================================\n");
    printf("           测试汇总\n");
    printf("========================================\n");
    printf("测试1 (基本寄存器读写):     %s\n", test_results[0] == 0 ? "通过" : "失败");
    printf("测试2 (多个重试值测试):     %s\n", test_results[1] == 0 ? "通过" : "失败");
    printf("测试3 (边界值测试):         %s\n", test_results[2] == 0 ? "通过" : "失败");
    printf("测试4 (连续读写性能):       %s\n", test_results[3] == 0 ? "通过" : "失败");
    printf("测试5 (寄存器回环测试):     %s\n", test_results[4] == 0 ? "通过" : "失败");
    printf("========================================\n");
    
    // 返回总的测试结果
    ret = test_results[0] | test_results[1] | test_results[2] | 
           test_results[3] | test_results[4];
    
    if (ret == 0) {
        printf("所有测试通过！\n");
    } else {
        printf("部分测试失败！\n");
    }
    
    return ret;
}
