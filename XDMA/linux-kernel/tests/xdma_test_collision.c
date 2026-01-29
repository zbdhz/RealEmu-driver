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

// 定义与Bluespec结构体对齐的数据结构
// #pragma pack(push, 1) // 禁用内存对齐，确保与FPGA侧严格匹配

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

// #pragma pack(pop) // 恢复默认对齐


#define BUFFER_SIZE 64
#define DEVICE_H2C "/dev/xdma0_h2c_0" // Host-to-Card 通道设备文件
#define DEVICE_C2H "/dev/xdma0_c2h_0" // Card-to-Host 通道设备文件
#define SEND_DELTA_TIME 500
#define SEND_NUM 1000

// 测试参数配置
#define DISTURB_RATE_MIN 0
#define DISTURB_RATE_MAX 100
#define DISTURB_RATE_STEP 10
#define DISTURB_POWER_MIN 0
#define DISTURB_POWER_MAX 100
#define DISTURB_POWER_STEP 10

// 测试结果结构体
typedef struct {
    int disturb_rate;
    int disturb_power;
    int received_packets;
} TestResult;

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

/**
 * @brief 执行单个测试点的测试
 * @param disturb_rate 干扰率 (0-100)
 * @param disturb_power 干扰功率
 * @return 接收到的包数，-1表示测试失败
 */
int run_single_test(int disturb_rate, int disturb_power) {
    int h2c_fd = open(DEVICE_H2C, O_RDWR);
    int c2h_fd = open(DEVICE_C2H, O_RDWR);

    if (h2c_fd < 0 || c2h_fd < 0) {
        perror("Failed to open XDMA device");
        return -1;
    }

    size_t buf_size = 64;
    uint8_t *rx_buf = (uint8_t*)aligned_alloc(4096, buf_size);
    uint8_t *tx_buf1 = (uint8_t*)aligned_alloc(4096, buf_size);
    uint8_t *tx_buf2 = (uint8_t*)aligned_alloc(4096, buf_size);

    if (!rx_buf || !tx_buf1 || !tx_buf2) {
        perror("Memory allocation failed");
        close(h2c_fd);
        close(c2h_fd);
        return -1;
    }
    memset(tx_buf1, 0, buf_size);
    memset(tx_buf2, 0, buf_size);

    // 创建主数据包
    MacBridge_TOP bridge_data1 = {
        .bridgeTag = {
            .control = 0,
            .notUsed = 0
        },
        .macEvent = {
            .srcMacId = 1,
            .dstMacId = 0,
            .rfParam = {
                .power = 50 + 578,
                .mcs = 0
            },
            .mpduDigest = {
                .frametype = 2,
                .framesubtype = 0,
                .duration = 0,
                .mpdulen = 256,
                .mpducacheaddr = 0
            },
            .status = 0,
        }
    };

    // 创建干扰包
    MacBridge_TOP bridge_data2 = {
        .bridgeTag = {
            .control = 0,
            .notUsed = 0
        },
        .macEvent = {
            .srcMacId = 2,
            .dstMacId = 0,
            .rfParam = {
                .power = disturb_power + 578,
                .mcs = 0
            },
            .mpduDigest = {
                .frametype = 2,
                .framesubtype = 0,
                .duration = 0,
                .mpdulen = 256,
                .mpducacheaddr = 0
            },
            .status = 0,
        }
    };

    // 准备干扰包数据
    uint8_t buffer2[BUFFER_SIZE] = {0};
    direct_reverse_mac_bridge_to_buffer(&bridge_data2, buffer2);
    memcpy(tx_buf2, &buffer2, sizeof(buffer2));

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        close(h2c_fd);
        close(c2h_fd);
        free(tx_buf1);
        free(tx_buf2);
        free(rx_buf);
        return -1;
    }

    pid_t pid = fork();
    
    if (pid == 0) {
        // 子进程：接收数据
        close(pipefd[0]); // 关闭读端
        printf("接收进程启动, PID: %d, disturb_rate=%d, disturb_power=%d\n", 
               getpid(), disturb_rate, disturb_power);

        int received_count = 0;
        MacBridge_TOP *event = (MacBridge_TOP*)malloc(sizeof(MacBridge_TOP));
        int seq_num = -1;
        
        while(1) {
            ssize_t read_bytes = read(c2h_fd, rx_buf, buf_size);
            if (read_bytes <= 0) {
                printf("读取失败或连接关闭，退出接收进程\n");
                break;
            }
            
            buffer_to_mac_bridge(rx_buf, event);
            if(event->macEvent.srcMacId == 1 && (int)event->macEvent.mpduDigest.mpducacheaddr > seq_num) {
                seq_num = event->macEvent.mpduDigest.mpducacheaddr;
                ++received_count;
            }
            
            if(received_count % 100 == 0) {
                printf("disturb_rate=%d, disturb_power=%d, Received: %d\n", 
                       disturb_rate, disturb_power, received_count);
            }
        }
        
        free(event);
        
        // 通过管道发送接收包数
        if (write(pipefd[1], &received_count, sizeof(received_count)) != sizeof(received_count)) {
            perror("pipe write failed");
        }
        close(pipefd[1]);
        exit(0);
        
    } else if (pid > 0) {
        // 父进程：发送数据
        sleep(1); // 确保子进程已就绪
        printf("发送进程启动, PID: %d, disturb_rate=%d, disturb_power=%d\n", 
               getpid(), disturb_rate, disturb_power);
        
        for(int i = 0; i < SEND_NUM; i++) {
            // 根据干扰率发送干扰包
            if(i % 100 < disturb_rate) {
                ssize_t written2 = write(h2c_fd, tx_buf2, buf_size);
                if (written2 < 0) {
                    perror("干扰包发送失败");
                }
            }

            // 发送主数据包
            uint8_t buffer1[BUFFER_SIZE] = {0};
            bridge_data1.macEvent.mpduDigest.mpducacheaddr = i;
            direct_reverse_mac_bridge_to_buffer(&bridge_data1, buffer1);
            memcpy(tx_buf1, &buffer1, sizeof(buffer1));
            
            ssize_t written = write(h2c_fd, tx_buf1, buf_size);
            if (written < 0) {
                perror("主数据包发送失败");
                break;
            }
            
            usleep(SEND_DELTA_TIME);
        }
        
        printf("发送进程完成，等待接收进程结束...\n");
        int status;
        wait(&status);
        
        int received_total = 0;
        if (WIFEXITED(status)) {
            // 从管道读取接收包数
            if (read(pipefd[0], &received_total, sizeof(received_total)) == sizeof(received_total)) {
                printf("测试完成: disturb_rate=%d, disturb_power=%d, 接收包数=%d/%d\n", 
                       disturb_rate, disturb_power, received_total, SEND_NUM);
            } else {
                perror("pipe read failed");
                received_total = -1;
            }
        }
        
        close(pipefd[0]);
        close(h2c_fd);
        close(c2h_fd);
        free(tx_buf1);
        free(tx_buf2);
        free(rx_buf);
        
        return received_total;
    }
    
    return -1;
}

/**
 * @brief 保存测试结果到CSV文件
 * @param results 测试结果数组
 * @param num_results 结果数量
 * @param filename 文件名
 */
void save_results_to_csv(TestResult* results, int num_results, const char* filename) {
    FILE* csv_file = fopen(filename, "w");
    if (!csv_file) {
        perror("无法打开CSV文件");
        return;
    }
    
    // 写入CSV标题
    fprintf(csv_file, "DISTURB_RATE,DISTURB_POWER,RECEIVED_PACKETS\n");
    
    // 写入数据
    for (int i = 0; i < num_results; i++) {
        fprintf(csv_file, "%d,%d,%d\n", 
                results[i].disturb_rate, 
                results[i].disturb_power, 
                results[i].received_packets);
    }
    
    fclose(csv_file);
    printf("测试结果已保存到: %s\n", filename);
}


int main() {
    printf("=== 碰撞测试开始 ===\n");
    printf("测试参数范围:\n");
    printf("DISTURB_RATE: %d-%d (步长%d)\n", 
           DISTURB_RATE_MIN, DISTURB_RATE_MAX, DISTURB_RATE_STEP);
    printf("DISTURB_POWER: %d-%d (步长%d)\n", 
           DISTURB_POWER_MIN, DISTURB_POWER_MAX, DISTURB_POWER_STEP);
    printf("每个测试点发送包数: %d\n", SEND_NUM);
    printf("==================\n\n");
    
    // 计算测试点数量
    int rate_steps = (DISTURB_RATE_MAX - DISTURB_RATE_MIN) / DISTURB_RATE_STEP + 1;
    int power_steps = (DISTURB_POWER_MAX - DISTURB_POWER_MIN) / DISTURB_POWER_STEP + 1;
    int total_tests = rate_steps * power_steps;
    
    printf("总共需要测试 %d 个点\n", total_tests);
    
    // 分配结果数组
    TestResult* results = (TestResult*)malloc(total_tests * sizeof(TestResult));
    if (!results) {
        perror("内存分配失败");
        return -1;
    }
    
    int result_index = 0;
    
    // 二重循环测试
    for (int disturb_rate = DISTURB_RATE_MIN; disturb_rate <= DISTURB_RATE_MAX; disturb_rate += DISTURB_RATE_STEP) {
        for (int disturb_power = DISTURB_POWER_MIN; disturb_power <= DISTURB_POWER_MAX; disturb_power += DISTURB_POWER_STEP) {
            printf("\n=== 开始测试: DISTURB_RATE=%d, DISTURB_POWER=%d ===\n", 
                   disturb_rate, disturb_power);
            
            int received_packets = run_single_test(disturb_rate, disturb_power);
            
            // 保存结果
            results[result_index].disturb_rate = disturb_rate;
            results[result_index].disturb_power = disturb_power;
            results[result_index].received_packets = received_packets;
            
            printf("测试完成: DISTURB_RATE=%d, DISTURB_POWER=%d, 接收包数=%d/%d\n", 
                   disturb_rate, disturb_power, received_packets, SEND_NUM);
            
            result_index++;
        }
    }
    
    // 保存结果到CSV文件
    save_results_to_csv(results, total_tests, "collision_test_results.csv");
    
    // 打印汇总结果
    printf("\n=== 测试汇总 ===\n");
    for (int i = 0; i < total_tests; i++) {
        printf("DISTURB_RATE=%3d, DISTURB_POWER=%3d, 接收包数=%4d/%d\n", 
               results[i].disturb_rate, results[i].disturb_power, 
               results[i].received_packets, SEND_NUM);
    }
    
    free(results);
    printf("\n=== 碰撞测试完成 ===\n");
    
    return 0;
    
    return 0;
}

