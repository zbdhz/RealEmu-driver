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
#define SNED_DELTA_TIME 10
#define SEND_NUM 1000

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
// #define BURST_SIZE 1


int main() {
    int h2c_fd = open(DEVICE_H2C, O_RDWR); // 打开 H2C 设备
    int c2h_fd = open(DEVICE_C2H, O_RDWR); // 打开 C2H 设备

    if (h2c_fd < 0 || c2h_fd < 0) {
        perror("Failed to open XDMA device");
        return -1;
    }
    printf("open success!\n");
    // 修改缓冲区分配和初始化
    size_t buf_size = 64; 
    uint8_t *rx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 使用更大的对齐
    uint8_t *tx_buf = (uint8_t*)aligned_alloc(4096, buf_size); // 改为uint8_t类型
    if (!rx_buf || !tx_buf) {
        perror("Memory allocation failed");
        close(h2c_fd);
        close(c2h_fd);
        return -1;
    }
    memset(tx_buf, 0, buf_size);
    // 创建 MacBridge_TOP 格式的数据
    MacBridge_TOP bridge_data = {
        .bridgeTag = {
            .control = 0,    // 控制标志设为0，表示这是一个MacEvent //66
            .notUsed = 0     // 未使用位清零 //67
        },
        .macEvent = {
            .srcMacId = 63,   // 源MAC ID //74
            .dstMacId = 0,   // 目标MAC ID设为1 //84
            .rfParam = {
                .power = 578 + 32*2,  // 功率设为最大值 //98
                .mcs = 0       // MCS设为0 //110
            },
            .mpduDigest = {
                .frametype = 2,     // 帧w类型
                .framesubtype = 0,  // 帧子类型
                .duration = 0,      // 持续时间
                .mpdulen = 1,        // 长度
                .mpducacheaddr = 0      // 缓存地址
            },
            .status = 0,        // 状态
        }
    };

    // 创建 CfgBridge_TOP 格式的数据，用于配置节点0和节点1之间的距离为1
    CfgBridge_TOP cfg_bridge_data = {
        .bridgeTag = {
            .control = 1,    // 控制标志设为1，表示这是一个配置消息
            .notUsed = 127    // 未使用位清零
        },
        .channelCfg = {
            .srcPhyId = 0,   // 源物理ID为0
            .dstPhyId = 1,   // 目标物理ID为1
            .distance = 8    // 距离设为10
        }
    };
    uint8_t buffer[BUFFER_SIZE] = {0};
    direct_reverse_mac_bridge_to_buffer(&bridge_data, buffer);
    // direct_reverse_cfg_bridge_to_buffer(&cfg_bridge_data, buffer);
    
    // print_buffer_in_binary(buffer,64);
    memcpy(tx_buf, &buffer, sizeof(buffer)); // 将数据复制到tx_buf
    
    // 确保结构体大小与传输大小匹配
    if (sizeof(MacEvent) > buf_size) {
        printf("MacEvent size %zu exceeds buffer size %zu\n", sizeof(MacEvent), buf_size);
        close(h2c_fd);
        close(c2h_fd);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

        // 通过 C2H 通道接收数据
	if (fork() == 0) {
        printf("接收进程启动, PID: %d\n", getpid());

        int received_count = 0;
        MacBridge_TOP *event = (MacBridge_TOP*)malloc(sizeof(MacBridge_TOP));
        while(received_count < SEND_NUM)
            {
                ssize_t read_bytes = read(c2h_fd, rx_buf, buf_size);
                if (read_bytes <= 0) {
                    printf("读取失败或连接关闭，退出接收进程\n");
                    break;
                }
                ++received_count;
                if(received_count%1==0)
                {
                    printf("Received %zd bytes from FPGA, total Received: %d \n", read_bytes, received_count);
                    print_current_time();
                }
                buffer_to_mac_bridge(rx_buf, event);
                // printf("  MacEvent data:\n");
                // printf("  srcMacId: 0x%x\n", event->macEvent.srcMacId);
                // printf("  dstMacId: 0x%x\n", event->macEvent.dstMacId);
                printf("  rfParam.power: %u\n", event->macEvent.rfParam.power);
                // printf("  rfParam.mcs: %u\n", event->macEvent.rfParam.mcs);
                // printf("  mpduDigest.frametype: %u\n", event->macEvent.mpduDigest.frametype);
                // printf("  mpduDigest.mpducacheaddr: 0x%lx\n", event->macEvent.mpduDigest.mpducacheaddr);
            }
        free(event);
        return 0;
        }
    else {
        sleep(1);  // 确保子进程的 read 已就绪
        printf("发送进程启动, PID: %d\n", getpid());
        for(int i=0;i<SEND_NUM;i++){
            ssize_t written = write(h2c_fd, tx_buf, buf_size);

        if (written < 0) {
            perror("H2C write failed");
            printf("Error details: %s\n", strerror(errno));
            break;
        } else {
            if(i%1==0)
            {
                // printf("Pkt_id:%d, total_pkt: %d,Sent %zd bytes to FPGA\n",i,SEND_NUM, written);
                // print_current_time();

            }
            // printf("Pkt_id:%d, total_pkt: %d,Sent %zd bytes to FPGA\n",i,SEND_NUM, written);
        }
        usleep(1000);
        }
        printf("发送进程完成，等待接收进程结束...\n");
        int status;
        wait(&status);  // 等待子进程结束
        sleep(1);
    }	
        
    // 验证数据一致性
    close(h2c_fd);
    close(c2h_fd);
    free(tx_buf);
    free(rx_buf);
    
    return 0;
}

