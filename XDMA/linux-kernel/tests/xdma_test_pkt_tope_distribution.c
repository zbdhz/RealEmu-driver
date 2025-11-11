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

#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>

// 定义与Bluespec结构体对齐的数据结构
#pragma pack(push, 1) // 禁用内存对齐，确保与FPGA侧严格匹配

// BridgeTag 结构体
typedef struct {
    uint8_t notUsed:7;     // 7位 (NOTUSED_FLAG_WIDTH)
    uint8_t control:1;     // 1位 (CONTROL_FLAG_WIDTH)
} BridgeTag;

// MacEvent 结构体
typedef struct {
    uint8_t status:1;       //1位
    struct {
        uint64_t mpducacheaddr:64; //64位
        uint16_t mpdulen:16;   //16位
        uint16_t duration:16;  //16位
        uint16_t  framesubtype:4;  //4位
        uint16_t frametype:2;  // 2位
    } mpduDigest;
    struct {
        uint16_t mcs:4;     //4位
        uint16_t power:12;   //12位
    } rfParam;
    uint16_t dstMacId:10;  //10位
    uint16_t srcMacId:10;  //10位
} MacEvent;

// MacBridge_TOP 结构体
typedef struct {
    MacEvent macEvent;     // MacEvent 结构体
    BridgeTag bridgeTag;   // BridgeTag 结构体
} MacBridge_TOP;

// ChannelCfg 结构体
typedef struct {
    uint16_t distance:10;     // 10位 (NodeDistance)
    uint16_t dstPhyId:10;     // 10位 (PhyId 来自 MacId)
    uint16_t srcPhyId:10;     // 10位 (PhyId 来自 MacId)
} ChannelCfg;

// CfgBridge_TOP 结构体
typedef struct {
    ChannelCfg channelCfg;
    BridgeTag bridgeTag;   // BridgeTag 结构体
} CfgBridge_TOP;

#pragma pack(pop) // 恢复默认对齐


#define BUFFER_SIZE 64

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

// 任意长度下的缓存区设置缓冲区中指定位置的位
void set_bits(uint8_t* buffer, size_t start_bit, size_t num_bits, uint64_t value) {
    for (size_t i = 0; i < num_bits; i++) {
        set_bit(buffer, start_bit + i, (value >> i) & 0x1);
    }
}

//将MacBridge_TOP结构体数据转化为二进制缓冲区
void direct_reverse_mac_bridge_to_buffer(const MacBridge_TOP* data, uint8_t* buffer) {
    size_t bit_pos = 0;
    // 从最低位开始反向设置
    set_bits(buffer, bit_pos, 7, data->bridgeTag.notUsed);
    bit_pos += 7;
    set_bit(buffer, bit_pos++, data->bridgeTag.control);
    set_bit(buffer, bit_pos++, data->macEvent.status);
    set_bits(buffer, bit_pos, 64, data->macEvent.mpduDigest.mpducacheaddr);
    bit_pos += 64;
    set_bits(buffer, bit_pos, 16, data->macEvent.mpduDigest.mpdulen);
    bit_pos += 16;
    set_bits(buffer, bit_pos, 16, data->macEvent.mpduDigest.duration);
    bit_pos += 16;
    set_bits(buffer, bit_pos, 4, data->macEvent.mpduDigest.framesubtype);
    bit_pos += 4;
    set_bits(buffer, bit_pos, 2, data->macEvent.mpduDigest.frametype);
    bit_pos += 2;
    set_bits(buffer, bit_pos, 4, data->macEvent.rfParam.mcs);
    bit_pos += 4;
    set_bits(buffer, bit_pos, 12, data->macEvent.rfParam.power);
    bit_pos += 12;
    set_bits(buffer, bit_pos, 10, data->macEvent.dstMacId);
    bit_pos += 10;
    set_bits(buffer, bit_pos, 10, data->macEvent.srcMacId);
    bit_pos += 10;
}

//将CfgBridge_TOP结构体数据转化为二进制缓冲区
void direct_reverse_cfg_bridge_to_buffer(const CfgBridge_TOP* data, uint8_t* buffer) {
    size_t bit_pos = 0;
    // 从最低位开始反向设置
    set_bits(buffer, bit_pos, 1, data->bridgeTag.notUsed);
    bit_pos += 7;
    set_bit(buffer, bit_pos++, data->bridgeTag.control);
    set_bits(buffer, bit_pos, 10, data->channelCfg.distance);
    bit_pos += 10;    
    set_bits(buffer, bit_pos, 10, data->channelCfg.dstPhyId);
    bit_pos += 10;
    set_bits(buffer, bit_pos, 10, data->channelCfg.srcPhyId);
    bit_pos += 10;
}

// 转化单个字节到二进制字符串，用于调试打印
void print_byte_in_binary(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        printf("%d", (byte >> i) & 1);
    }
    printf("\n");
}

// 转化缓冲区中的数据到二进制字符串，用于调试打印
void print_buffer_in_binary(const uint8_t* buffer, size_t buffer_size) {
    for (size_t i = 0; i < buffer_size; i++) {
        print_byte_in_binary(buffer[i]);
        printf(" "); 
    }
    printf("\n");
}

/**
 * @brief 将缓冲区中的数据反向解析为 MacBridge_TOP 结构体
 * @param buffer 输入的缓冲区，包含按位存储的数据
 * @param data 输出的 MacBridge_TOP 结构体指针
 */
void buffer_to_mac_bridge(const uint8_t* buffer, MacBridge_TOP* data) {
    size_t bit_pos = 0;

    // 暂未在返回的数据包中添加控制帧，后续可以添加，则需更新转换函数
    // // 解析 notUsed 字段 (1位)
    // data->bridgeTag.notUsed = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    // bit_pos += 1;

    // // 解析 control 字段 (1位)
    // data->bridgeTag.control = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    // bit_pos += 1;

    // 解析 status 字段 (1位)
    data->macEvent.status = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    bit_pos += 1;

    // 解析 mpducacheaddr 字段 (64位)
    uint64_t mpducacheaddr = 0;
    for (int i = 0; i < 64; i++) {
        mpducacheaddr |= ((uint64_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.mpducacheaddr = mpducacheaddr;
    bit_pos += 64;

    // 解析 mpdulen 字段 (16位)
    uint16_t mpdulen = 0;
    for (int i = 0; i < 16; i++) {
        mpdulen |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.mpdulen = mpdulen;
    bit_pos += 16;

    // 解析 duration 字段 (16位)
    uint16_t duration = 0;
    for (int i = 0; i < 16; i++) {
        duration |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.duration = duration;
    bit_pos += 16;

    // 解析 framesubtype 字段 (4位)
    uint16_t framesubtype = 0;
    for (int i = 0; i < 4; i++) {
        framesubtype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.framesubtype = framesubtype;
    bit_pos += 4;

    // 解析 frametype 字段 (2位)
    uint16_t frametype = 0;
    for (int i = 0; i < 2; i++) {
        frametype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.frametype = frametype;
    bit_pos += 2;

    // 解析 mcs 字段 (4位)
    uint16_t mcs = 0;
    for (int i = 0; i < 4; i++) {
        mcs |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.rfParam.mcs = mcs;
    bit_pos += 4;

    // 解析 power 字段 (12位)
    uint16_t power = 0;
    for (int i = 0; i < 12; i++) {
        power |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.rfParam.power = power;
    bit_pos += 12;

    // 解析 dstMacId 字段 (10位)
    uint16_t dstMacId = 0;
    for (int i = 0; i < 10; i++) {
        dstMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.dstMacId = dstMacId;
    bit_pos += 10;

    // 解析 srcMacId 字段 (10位)
    uint16_t srcMacId = 0;
    for (int i = 0; i < 10; i++) {
        srcMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.srcMacId = srcMacId;
    bit_pos += 10;
}

void print_current_time() {
    struct timeval tv;
    struct tm *local_time;
    
    // 获取当前时间（微秒精度）
    gettimeofday(&tv, NULL);
    
    // 转换为本地时间
    local_time = localtime(&tv.tv_sec);
    
    // 打印格式化时间，包含微秒
    printf("当前时间: %04d-%02d-%02d %02d:%02d:%02d.%06ld\n",
           local_time->tm_year + 1900,
           local_time->tm_mon + 1,
           local_time->tm_mday,
           local_time->tm_hour,
           local_time->tm_min,
           local_time->tm_sec,
           tv.tv_usec);
}

// CSV数据结构体
typedef struct {
    int time_sec;       // 时刻（秒数）
    int src_id;         // 源ID
    int dst_id;         // 目的ID
    int distance;       // 距离
} CsvChannelConfig;

/**
 * @brief 从CSV文件中读取特定秒数的信道配置数据
 * @param file_path CSV文件路径
 * @param target_sec 要读取的目标秒数
 * @param configs 输出参数，存储读取到的配置数据
 * @param max_configs 最大配置数量
 * @return 实际读取到的配置数量，-1表示错误
 */
int read_csv_configs_for_second(const char* file_path, int target_sec, 
                               CsvChannelConfig* configs, int max_configs) {
    FILE* file = fopen(file_path, "r");
    if (!file) {
        perror("无法打开CSV文件");
        return -1;
    }
    
    char line[256];
    int count = 0;
    
    // 跳过标题行（如果有）
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }
    
    // 检查是否是标题行（包含非数字字符）
    int is_header = 0;
    char* token = strtok(line, ",");
    if (token && !isdigit(token[0])) {
        is_header = 1;
    }
    
    // 如果不是标题行，需要重置文件指针并处理第一行数据
    if (!is_header) {
        rewind(file);
    }
    
    // 逐行读取CSV文件
    while (fgets(line, sizeof(line), file) != NULL && count < max_configs) {
        int time_sec, src_id, dst_id, distance;
        
        // 解析CSV行
        if (sscanf(line, "%d,%d,%d,%d", &time_sec, &src_id, &dst_id, &distance) == 4) {
            // 检查是否是目标秒数
            if (time_sec == target_sec) {
                configs[count].time_sec = time_sec;
                configs[count].src_id = src_id;
                configs[count].dst_id = dst_id;
                configs[count].distance = distance;
                count++;
            }else{
                // break;
            }
        }
    }
    
    fclose(file);
    return count;
}

/**
 * @brief 下发信道配置到FPGA
 * @param h2c_fd H2C设备文件描述符
 * @param configs 信道配置数组
 * @param config_count 配置数量
 * @param tx_buf 发送缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功下发的配置数量，-1表示错误
 */
int send_channel_configs(int h2c_fd, const CsvChannelConfig* configs, 
                        int config_count, uint8_t* tx_buf, size_t buf_size) {
    int success_count = 0;
    
    for (int i = 0; i < config_count; i++) {
        // 创建 CfgBridge_TOP 格式的数据
        CfgBridge_TOP cfg_bridge_data = {
            .bridgeTag = {
                .control = 1,    // 控制标志设为1，表示这是一个配置消息
                .notUsed = 127   // 未使用位清零
            },
            .channelCfg = {
                .srcPhyId = configs[i].src_id,   // 源物理ID
                .dstPhyId = configs[i].dst_id,   // 目标物理ID
                .distance = configs[i].distance  // 距离
            }
        };
        
        // 序列化数据
        uint8_t cfg_buffer[BUFFER_SIZE] = {0};
        direct_reverse_cfg_bridge_to_buffer(&cfg_bridge_data, cfg_buffer);
        memcpy(tx_buf, cfg_buffer, sizeof(cfg_buffer));
        
        // 发送配置
        ssize_t written = write(h2c_fd, tx_buf, buf_size);
        if (written < 0) {
            perror("信道配置发送失败");
            printf("配置 %d (src=%d, dst=%d, distance=%d) 发送失败\n", 
                   i, configs[i].src_id, configs[i].dst_id, configs[i].distance);
        } else {
            printf("配置 %d (src=%d, dst=%d, distance=%d) 发送成功\n", 
                   i, configs[i].src_id, configs[i].dst_id, configs[i].distance);
            success_count++;
        }
        
        // 短暂延迟，确保配置被处理
        usleep(100);  // 0.1m实测可用
    }
    
    return success_count;
}

/**
 * @brief 处理CSV文件中的信道配置
 * @param file_path CSV文件路径
 * @param h2c_fd H2C设备文件描述符
 * @param tx_buf 发送缓冲区
 * @param buf_size 缓冲区大小
 * @param start_sec 开始处理的秒数
 * @param end_sec 结束处理的秒数
 * @return 成功处理的配置总数，-1表示错误
 */
int process_csv_configs(const char* file_path, int h2c_fd, 
                       uint8_t* tx_buf, size_t buf_size,
                       int start_sec, int end_sec) {
    int total_success = 0;
    
    // 为每个秒数分配配置数组
    CsvChannelConfig* configs = malloc(100 * sizeof(CsvChannelConfig));  // 假设每秒最多100个配置
    
    if (!configs) {
        perror("内存分配失败");
        return -1;
    }
    
    // 逐秒处理配置
    for (int sec = start_sec; sec <= end_sec; sec++) {
        printf("\n=== 处理第 %d 秒的配置 ===\n", sec);
        
        // 读取当前秒的配置
        int config_count = read_csv_configs_for_second(file_path, sec, configs, 100);
        
        if (config_count < 0) {
            printf("读取第 %d 秒的配置失败\n", sec);
            free(configs);
            return -1;
        } else if (config_count == 0) {
            printf("第 %d 秒没有配置数据\n", sec);
            continue;
        }
        
        printf("读取到 %d 个配置\n", config_count);
        
        // 发送配置
        int success_count = send_channel_configs(h2c_fd, configs, config_count, tx_buf, buf_size);
        
        if (success_count < 0) {
            printf("发送第 %d 秒的配置失败\n", sec);
            free(configs);
            return -1;
        }
        
        total_success += success_count;
        printf("第 %d 秒的配置处理完成，成功发送 %d/%d 个配置\n", sec, success_count, config_count);
        
        // 等待配置生效
        sleep(1);
    }
    
    free(configs);
    return total_success;
}

#define DEVICE_H2C "/dev/xdma0_h2c_0" // Host-to-Card 通道设备文件
#define DEVICE_C2H "/dev/xdma0_c2h_0" // Card-to-Host 通道设备文件
#define BURST_SIZE 1

// #define CSV_FILE_PATH "channel_config.csv"  // 修改为您的CSV文件路径
// #define START_SECOND 1                            // 修改为开始处理的秒数
// #define END_SECOND 1                             // 修改为结束处理的秒数
// #define MAX_CONFIGS_PER_SECOND 200               // 修改为每秒最大配置数量

#define CSV_FILE_PATH "channel_config.csv"
#define START_SECOND 1
#define END_SECOND 1
#define MAX_CONFIGS_PER_SECOND 56

int main() {
    printf("=== 信道配置程序 ===\n");
    printf("CSV文件路径: %s\n", CSV_FILE_PATH);
    printf("处理秒数范围: %d-%d\n", START_SECOND, END_SECOND);
    printf("===================\n\n");

    int h2c_fd = open(DEVICE_H2C, O_RDWR); // 打开 H2C 设备
    // int c2h_fd = open(DEVICE_C2H, O_RDWR); // 打开 C2H 设备

    if (h2c_fd < 0) {
        perror("Failed to open XDMA device");
        return -1;
    }
    printf("open success!\n");
    // 修改缓冲区分配和初始化
    size_t buf_size = 64; 
    // uint8_t *rx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 使用更大的对齐
    uint8_t *tx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 改为uint8_t类型
    if (!tx_buf) {
        perror("Memory allocation failed");
        close(h2c_fd);
        // close(c2h_fd);
        return -1;
    }
    memset(tx_buf, 0, buf_size);
        // 处理CSV配置
    int total_configs = process_csv_configs(CSV_FILE_PATH, h2c_fd, tx_buf, buf_size, 
                                          START_SECOND, END_SECOND);
    
    if (total_configs >= 0) {
        printf("\n配置处理完成, 总共成功发送 %d 个配置\n", total_configs);
    } else {
        printf("\n配置处理失败\n");
    }

    //清理资源
    close(h2c_fd);
    // close(c2h_fd);
    free(tx_buf);
    // free(rx_buf);
    
    return 0;
}

