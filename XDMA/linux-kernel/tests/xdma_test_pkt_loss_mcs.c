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
#include <sys/stat.h>  // 用于chmod函数和权限常量
#include <grp.h>       // 用于getgrnam函数

#define BUFFER_SIZE 64
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

#define DEVICE_H2C "/dev/xdma0_h2c_0" // Host-to-Card 通道设备文件
#define DEVICE_C2H "/dev/xdma0_c2h_0" // Card-to-Host 通道设备文件
#define BURST_SIZE 1
#define OUTPUT_CSV_FILE_PREFIX "sinr_success_rate_scaled_mcs"  // 输出CSV文件名前缀
#define TEST_MCS 7              // MCS值 (0-7)
#define TEST_POWER_START 32*19      // 起始功率值
#define TEST_POWER_STEP 4         // 功率步进值
#define TEST_POWER_STEPS 32       // 功率步进次数
#define PACKETS_PER_STEP 1000     // 每个功率点的测试包数
#define SEND_DELTA_TIME 500 //us

// 添加函数用于生成动态文件名
char* generate_csv_filename(int mcs) {
    // 分配足够的内存来存储文件名
    char* filename = malloc(100 * sizeof(char));
    if (!filename) {
        return NULL;
    }
    
    // 格式化文件名，包含MCS值
    snprintf(filename, 100, "%s%d.csv", OUTPUT_CSV_FILE_PREFIX, mcs);
    
    return filename;
}

// 执行单个功率点的测试
int run_single_test(int h2c_fd, int c2h_fd, uint8_t* tx_buf, uint8_t* rx_buf, size_t buf_size, int mcs, int power, int packet_count) {
    int received_count = 0;
    
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        return -1;
    }

    pid_t pid = fork();
    
    if (pid == 0) {
        // 子进程：接收数据
        close(pipefd[0]); // 子进程关闭读端
        MacBridge_TOP *event = (MacBridge_TOP*)malloc(sizeof(MacBridge_TOP));
        if (!event) {
            perror("内存分配失败");
            exit(EXIT_FAILURE);
        }
        
        while (received_count < packet_count) {
            ssize_t read_bytes = read(c2h_fd, rx_buf, buf_size);
            if (read_bytes <= 0) {
                break; //发送完成 && 长时间未接收到数据，则退出循环
            }
            
            buffer_to_mac_bridge(rx_buf, event);
            received_count++;

        }
        
        free(event);
        printf("MCS=%d, Power=%d: 接收 %d/%d 包\n", mcs, power-578, received_count, packet_count);
        // ✅ 把真实接收包数写入管道
        if (write(pipefd[1], &received_count, sizeof(received_count)) != sizeof(received_count)) {
            perror("pipe write failed");
        }
        close(pipefd[1]);
        exit(0); // 退出子进程

    } else if (pid > 0) {
        // 父进程：发送数据
        sleep(1); // 确保子进程已就绪
        
        for (int i = 0; i < packet_count; i++) {
            ssize_t written = write(h2c_fd, tx_buf, buf_size);
            if (written < 0) {
                perror("H2C write failed");
                break;
            }
            usleep(SEND_DELTA_TIME); // 1ms延迟
        }
        int status;
        waitpid(pid, &status, 0);

        int received_total = 0;
        if (WIFEXITED(status)) {
            // 从管道读取真实接收包数
            if (read(pipefd[0], &received_total, sizeof(received_total)) == sizeof(received_total)) {
                close(pipefd[0]);
                return received_total;
            } else {
                perror("pipe read failed");
            }
        }
        close(pipefd[0]);
        return -1; // 出错
    }
}


int main() {
        // 生成CSV文件名
    char* csv_filename = generate_csv_filename(TEST_MCS);
    if (!csv_filename) {
        printf("内存分配失败\n");
        return -1;
    }

    // 打印测试配置 
    printf("=== 丢包测试配置 ===\n");
    printf("MCS值: %d\n", TEST_MCS);
    printf("起始功率: %d\n", TEST_POWER_START);
    printf("功率步进: %d\n", TEST_POWER_STEP);
    printf("功率步进次数: %d\n", TEST_POWER_STEPS);
    printf("每个功率点测试包数: %d\n", PACKETS_PER_STEP);
    printf("输出CSV文件: %s\n", csv_filename);
    printf("===================\n\n");

    // 打开CSV文件用于写入
    FILE* csv_file = fopen(csv_filename, "w");

    if (!csv_file) {
        perror("无法打开输出CSV文件");
        free(csv_filename);
        return -1;
    }
    // 设置文件权限，允许所有用户读写
    // chmod(csv_filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    // 或者更精确地，允许文件所有者和gtx用户读写
    chmod(csv_filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    chown(csv_filename, -1, getgrnam("gtx")->gr_gid);  // 将文件组改为gtx组
        // 写入CSV标题行
    // fprintf(csv_file, "Power(dBm),Send,Recv,SuccessRate\n");


    int h2c_fd = open(DEVICE_H2C, O_RDWR); // 打开 H2C 设备
    int c2h_fd = open(DEVICE_C2H, O_RDWR); // 打开 C2H 设备

    if (h2c_fd < 0 || c2h_fd < 0) {
        perror("Failed to open XDMA device");
        fclose(csv_file);
        free(csv_filename);
        return -1;
    }
    printf("XDMA设备打开成功!\n");

    // 修改缓冲区分配和初始化
    size_t buf_size = 64; 
    uint8_t *rx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 使用更大的对齐
    uint8_t *tx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 改为uint8_t类型

    if (!rx_buf || !tx_buf) {
        perror("Memory allocation failed");
        close(h2c_fd);
        close(c2h_fd);
        fclose(csv_file);
        free(csv_filename);
        return -1;
    }
    memset(tx_buf, 0, buf_size);

    // 创建 CfgBridge_TOP 格式的数据，用于配置节点0和节点1之间的距离为1
    CfgBridge_TOP cfg_bridge_data = {
        .bridgeTag = {
            .control = 1,    // 控制标志设为1，表示这是一个配置消息
            .notUsed = 127    // 未使用位清零
        },
        .channelCfg = {
            .srcPhyId = 0,   // 源物理ID为0
            .dstPhyId = 1,   // 目标物理ID为1
            .distance = 8    // 距离设为8
        }
    };

    uint8_t cfg_buffer[BUFFER_SIZE] = {0};
    direct_reverse_cfg_bridge_to_buffer(&cfg_bridge_data, cfg_buffer);
    memcpy(tx_buf, cfg_buffer, sizeof(cfg_buffer));
    ssize_t cfg_written = write(h2c_fd, tx_buf, buf_size);
    if (cfg_written < 0) {
        perror("信道配置发送失败");
    } else {
        printf("信道配置发送成功\n");
    }
    sleep(1); // 等待配置生效


    // 测试结果数组
    int* received_counts = malloc(TEST_POWER_STEPS * sizeof(int));
    if (!received_counts) {
        perror("内存分配失败");
        close(h2c_fd);
        close(c2h_fd);
        free(tx_buf);
        free(rx_buf);
        fclose(csv_file);
        free(csv_filename);
        return -1;
    }


    // 执行不同功率下的测试
    for (int step = 0; step < TEST_POWER_STEPS; step++) {
        int current_power = TEST_POWER_START + step * TEST_POWER_STEP + 578;
        
        printf("开始测试: MCS=%d, Power=%d\n", TEST_MCS, current_power);
        
        // 创建测试数据
        MacBridge_TOP bridge_data = {
            .bridgeTag = {
                .control = 0,
                .notUsed = 0
            },
            .macEvent = {
                .srcMacId = 0,
                .dstMacId = 1,
                .rfParam = {
                    .power = current_power,
                    .mcs = TEST_MCS
                },
                .mpduDigest = {
                    .frametype = 2,
                    .framesubtype = 0,
                    .duration = 0,
                    .mpdulen = 1,
                    .mpducacheaddr = 0
                },
                .status = 0
            }
        };
        
        // 序列化数据
        uint8_t buffer[BUFFER_SIZE] = {0};
        direct_reverse_mac_bridge_to_buffer(&bridge_data, buffer);
        memcpy(tx_buf, buffer, sizeof(buffer));
        
        // 执行测试
        received_counts[step] = run_single_test(h2c_fd, c2h_fd, tx_buf, rx_buf, 
                                              buf_size, TEST_MCS, current_power, 
                                              PACKETS_PER_STEP);
        
        // 计算丢包率
        float loss_rate = (1.0 - (float)received_counts[step] / PACKETS_PER_STEP) * 100.0;
        printf("结果: MCS=%d, Power=%d, 接收=%d/%d, 丢包率=%.2f%%\n\n", 
               TEST_MCS, current_power-578, received_counts[step], 
               PACKETS_PER_STEP, loss_rate);
        
        if(step == 0)
        {
            // 写入CSV标题行
            fprintf(csv_file, "Power(dBm),Send,Recv,SuccessRate\n");
        }
        
        // 将结果写入CSV文件
        float success_rate = (float)received_counts[step] / PACKETS_PER_STEP;
        fprintf(csv_file, "%.3f,%d,%d,%.2f\n", 
                (float)(current_power-578)/32.0, PACKETS_PER_STEP, received_counts[step], success_rate);
        
        // 确保数据立即写入文件
        fflush(csv_file);
    }
    // 关闭CSV文件
    fclose(csv_file);
    printf("测试结果已保存到CSV文件: %s\n", csv_filename);
    
    // 释放动态分配的内存
    free(csv_filename);

    //关闭打开的文件
    close(h2c_fd);
    close(c2h_fd);
    free(tx_buf);
    free(rx_buf);
    free(received_counts);
    
    return 0;
}

