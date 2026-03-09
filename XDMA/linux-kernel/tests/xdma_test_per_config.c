/**
 * @file xdma_test_per_config.c
 * @brief PER查找表配置下发程序
 * 
 * 该程序用于将Per.mem文件中的PER查找表数据下发到RealEmu硬件
 * 通过XDMA接口将数据写入硬件的PER配置寄存器
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

// 定义与Bluespec结构体对齐的数据结构
#pragma pack(push, 1)

// BridgeTag 结构体
typedef struct {
    uint8_t notUsed:7;     // 7位 (NOTUSED_FLAG_WIDTH)
    uint8_t control:1;     // 1位 (CONTROL_FLAG_WIDTH)
} BridgeTag;

// PerCfg 结构体 (PER_IN_WITDH=14, PER_OUT_WITDH=16)
typedef struct {
    uint16_t perIn:14;      // 14位 - PER查找表地址
    uint16_t perOut:16;     // 16位 - PER查找表值
} PerCfg;

// CfgBridge_TOP_Per 结构体
typedef struct {
    PerCfg perCfg;
    BridgeTag bridgeTag;
} CfgBridge_TOP_Per;

#pragma pack(pop)

#define BUFFER_SIZE 64
#define PER_TABLE_SIZE 8960  // 8 MCS * 1120 SINR samples
#define H2C_DEVICE "/dev/xdma0_h2c_0"

// 按照字节设置缓冲区中指定位置的位
void set_bit(uint8_t* buffer, size_t bit_pos, uint8_t value) {
    if (bit_pos >= BUFFER_SIZE * 8) return;
    
    size_t byte_pos = bit_pos / 8;
    size_t bit_in_byte = bit_pos % 8;
    
    if (value) {
        buffer[byte_pos] |= (1 << bit_in_byte);
    } else {
        buffer[byte_pos] &= ~(1 << bit_in_byte);
    }
}

// 设置缓冲区中指定位置的多个位
void set_bits(uint8_t* buffer, size_t start_bit, size_t num_bits, uint64_t value) {
    for (size_t i = 0; i < num_bits; i++) {
        set_bit(buffer, start_bit + i, (value >> i) & 0x1);
    }
}

// 将CfgBridge_TOP_Per结构体数据转化为二进制缓冲区
void per_cfg_to_buffer(const CfgBridge_TOP_Per* data, uint8_t* buffer) {
    size_t bit_pos = 0;
    
    // BridgeTag (8位)
    set_bits(buffer, bit_pos, 7, data->bridgeTag.notUsed);
    bit_pos += 7;
    set_bit(buffer, bit_pos++, data->bridgeTag.control);
    
    // PerCfg (30位)
    set_bits(buffer, bit_pos, 14, data->perCfg.perIn);   // perIn: 14位
    bit_pos += 14;
    set_bits(buffer, bit_pos, 16, data->perCfg.perOut);  // perOut: 16位
}

/**
 * @brief 读取Per.mem文件
 * @param file_path Per.mem文件路径
 * @param per_values 输出数组，存储读取的PER值
 * @param max_values 最大读取数量
 * @return 实际读取的数量，-1表示错误
 */
int read_per_mem_file(const char* file_path, uint16_t* per_values, int max_values) {
    FILE* file = fopen(file_path, "r");
    if (!file) {
        perror("无法打开Per.mem文件");
        return -1;
    }
    
    int count = 0;
    char line[16];
    
    while (fgets(line, sizeof(line), file) != NULL && count < max_values) {
        // 解析十六进制值
        unsigned int value;
        if (sscanf(line, "%x", &value) == 1) {
            per_values[count] = (uint16_t)(value & 0xFFFF);
            count++;
        }
    }
    
    fclose(file);
    printf("成功读取 %d 个PER值\n", count);
    return count;
}

/**
 * @brief 下发PER查找表到FPGA
 * @param h2c_fd H2C设备文件描述符
 * @param per_values PER值数组
 * @param per_count PER值数量
 * @param tx_buf 发送缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功下发的数量，-1表示错误
 */
int send_per_configs(int h2c_fd, const uint16_t* per_values, int per_count,
                     uint8_t* tx_buf, size_t buf_size) {
    int success_count = 0;
    
    printf("开始下发PER查找表，共 %d 个条目...\n", per_count);
    
    for (int i = 0; i < per_count; i++) {
        // 创建 CfgBridge_TOP_Per 格式的数据
        CfgBridge_TOP_Per per_cfg_data = {
            .bridgeTag = {
                .control = 1,    // 控制标志设为1
                .notUsed = 1     // notUsed设为1，表示PER配置
            },
            .perCfg = {
                .perIn = i,              // PER查找表地址
                .perOut = per_values[i]  // PER查找表值
            }
        };
        
        // 序列化数据
        memset(tx_buf, 0, buf_size);
        per_cfg_to_buffer(&per_cfg_data, tx_buf);
        
        // 发送配置
        ssize_t written = write(h2c_fd, tx_buf, buf_size);
        if (written < 0) {
            perror("PER配置发送失败");
            printf("配置 %d (地址=0x%04X, 值=0x%04X) 发送失败\n", 
                   i, i, per_values[i]);
        } else {
            success_count++;
            
            // 每1000个打印一次进度
            if ((i + 1) % 1000 == 0 || i == per_count - 1) {
                printf("进度: %d/%d (%.1f%%)\n", 
                       i + 1, per_count, (float)(i + 1) * 100 / per_count);
            }
        }
        
        // 短暂延迟，确保配置被处理
        usleep(10);  // 10微秒
    }
    
    printf("PER查找表下发完成: %d/%d 成功\n", success_count, per_count);
    return success_count;
}

/**
 * @brief 打印当前时间
 */
void print_current_time() {
    struct timeval tv;
    struct tm *local_time;
    
    gettimeofday(&tv, NULL);
    local_time = localtime(&tv.tv_sec);
    
    printf("当前时间: %04d-%02d-%02d %02d:%02d:%02d.%06ld\n",
           local_time->tm_year + 1900,
           local_time->tm_mon + 1,
           local_time->tm_mday,
           local_time->tm_hour,
           local_time->tm_min,
           local_time->tm_sec,
           tv.tv_usec);
}

void print_usage(const char* program_name) {
    printf("用法: %s <Per.mem文件路径> [H2C设备路径]\n", program_name);
    printf("示例:\n");
    printf("  %s /home/emu/dev/RealEmu/mem/Per.mem\n", program_name);
    printf("  %s /home/emu/dev/RealEmu/mem/Per.mem /dev/xdma0_h2c_0\n", program_name);
}

int main(int argc, char* argv[]) {
    printf("=== RealEmu PER查找表配置下发程序 ===\n");
    print_current_time();
    
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* per_mem_file = argv[1];
    const char* h2c_device = (argc >= 3) ? argv[2] : H2C_DEVICE;
    
    // 分配PER值数组
    uint16_t* per_values = (uint16_t*)malloc(PER_TABLE_SIZE * sizeof(uint16_t));
    if (!per_values) {
        perror("内存分配失败");
        return 1;
    }
    
    // 读取Per.mem文件
    printf("\n读取PER查找表文件: %s\n", per_mem_file);
    int per_count = read_per_mem_file(per_mem_file, per_values, PER_TABLE_SIZE);
    if (per_count < 0) {
        free(per_values);
        return 1;
    }
    
    if (per_count == 0) {
        printf("错误: PER文件为空\n");
        free(per_values);
        return 1;
    }
    
    // 打开H2C设备
    printf("\n打开H2C设备: %s\n", h2c_device);
    int h2c_fd = open(h2c_device, O_WRONLY);
    if (h2c_fd < 0) {
        perror("无法打开H2C设备");
        printf("请检查XDMA驱动是否加载，设备路径是否正确\n");
        free(per_values);
        return 1;
    }
    
    // 分配发送缓冲区
    uint8_t* tx_buf = (uint8_t*)malloc(BUFFER_SIZE);
    if (!tx_buf) {
        perror("发送缓冲区分配失败");
        close(h2c_fd);
        free(per_values);
        return 1;
    }
    
    // 下发PER查找表
    printf("\n");
    int success_count = send_per_configs(h2c_fd, per_values, per_count, 
                                         tx_buf, BUFFER_SIZE);
    
    // 清理资源
    free(tx_buf);
    free(per_values);
    close(h2c_fd);
    
    printf("\n");
    print_current_time();
    
    if (success_count == per_count) {
        printf("✅ PER查找表配置下发成功！\n");
        return 0;
    } else {
        printf("⚠️ PER查找表配置下发部分失败: %d/%d\n", success_count, per_count);
        return 1;
    }
}
