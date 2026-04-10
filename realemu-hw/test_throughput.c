#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "realemu_hw.h"
#include "../tools/reg_rw.h"
#include "../tools/dma_with_device.h"
#include "../tools/dma_utils.h"

#define MAX_NODES 16
#define DEFAULT_DISTANCE 1023
#define TEST_DISTANCE 8
#define MAX_QUEUE_SIZE 10000
#define TEST_DURATION 10 // 测试持续时间（秒）
#define PACKET_SIZE 1000 // 数据包大小（字节）
#define PACKET_SIZE_ADJUSTMENT 200 // 包长调整值

// MCS速率映射
typedef struct {
    int mcs;
    const char *rate_name;
} McsRateMap;

McsRateMap mcs_rate_map[] = {
    {0, "OfdmRate6Mbps"},
    {1, "OfdmRate9Mbps"},
    {2, "OfdmRate12Mbps"},
    {3, "OfdmRate18Mbps"},
    {4, "OfdmRate24Mbps"},
    {5, "OfdmRate36Mbps"},
    {6, "OfdmRate48Mbps"},
    {7, "OfdmRate54Mbps"}
};

#define MCS_COUNT (sizeof(mcs_rate_map) / sizeof(McsRateMap))

// 全局变量
volatile uint64_t packets_sent = 0;
volatile uint64_t packets_received = 0;
volatile uint64_t packets_received_statistics = 0; // 用于统计的收包数
volatile int running = 1;
volatile int statistics_running = 0; // 统计是否正在进行
struct timespec statistics_start_time;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

// 用于跟踪已收到的数据包序列号
#define MAX_SEQ_TRACK 1000000
uint64_t *received_seqs = NULL;
uint64_t seq_track_size = 0;

// 发送线程参数结构体
typedef struct {
    RealEmu_Device *realemu_device;
    int mcs;
} ThreadArgs;

// 获取当前时间戳（微秒）
uint64_t get_timestamp_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
}

// 节点0发送线程：向节点1发送数据包
void *node0_tx_thread_func(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    RealEmu_Device *realemu_device = args->realemu_device;
    int mcs = args->mcs;
    uint64_t packet_seq = 0;
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    printf("[Node0发送线程] 启动，向 Node1 发送数据包 (MCS=%d)...\n", mcs);

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed_sec = (current_time.tv_sec - start_time.tv_sec) +
                          (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

        if (elapsed_sec >= TEST_DURATION + 2) { // 发送时间比统计时间长2秒
            break;
        }

        // 生成数据包
        MacEvent tx_macevent;
        memset(&tx_macevent, 0, sizeof(MacEvent));
        tx_macevent.status = 1;
        tx_macevent.mpduDigest.mpducacheaddr = packet_seq;
        tx_macevent.mpduDigest.mpdulen = PACKET_SIZE + PACKET_SIZE_ADJUSTMENT; // 使用调整后的包长
        tx_macevent.mpduDigest.duration = 0;
        tx_macevent.mpduDigest.framesubtype = 0;
        tx_macevent.mpduDigest.frametype = 2;
        tx_macevent.rfParam.mcs = mcs;
        tx_macevent.rfParam.power = 2000;
        tx_macevent.dstMacId = 1;  // 发送给节点1
        tx_macevent.srcMacId = 0;

        // 发送数据包
        int ret = send_pkt_data(realemu_device, tx_macevent);
        if (ret == 0) {
            realemu_handle_tx_queue(realemu_device);
            
            pthread_mutex_lock(&stats_lock);
            packets_sent++;
            pthread_mutex_unlock(&stats_lock);

            if (packets_sent % 1000 == 0) {
                printf("[Node0发送线程] 已发送 %llu 个数据包\n", (unsigned long long)packets_sent);
            }
        }

        packet_seq++;
        // 短暂休眠，避免CPU占用过高
        usleep(1);
    }

    printf("[Node0发送线程] 发送完成，共发送 %llu 个数据包\n", (unsigned long long)packets_sent);
    
    // 发送完成，等待一段时间让所有数据包处理完毕
    printf("[Node0发送线程] 等待3秒让所有数据包处理完毕...\n");
    sleep(3);
    
    // 设置running为false，通知其他线程退出
    running = 0;
    
    return NULL;
}

// 节点1接收线程：接收节点0发送的数据包
void *node1_rx_thread_func(void *arg) {
    RealEmu_Device *realemu_device = (RealEmu_Device *)arg;
    MacEvent rx_macevent;
    int consecutive_empty = 0;
    int max_empty_count = 500;
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    printf("[Node1接收线程] 启动，等待接收 Node0 发送的数据包...\n");

    while (running) {
        int received_count = realemu_update_rx_queue(realemu_device);
        if (received_count < 0) {
            // 遇到错误，继续运行但记录错误
            fprintf(stderr, "[Node1接收线程] Error: Failed to update RX queue\n");
            usleep(10000);
            continue;
        }

        int ret;
        int packets_read = 0;

        do {
            ret = realemu_handle_rx_queue(realemu_device, &rx_macevent);
            if (ret == 1) {
                consecutive_empty = 0;
                packets_read++;

                // 检查是否是发给节点1的数据包
                if (rx_macevent.dstMacId == 1 && rx_macevent.srcMacId == 0) {
                    uint64_t packet_seq = rx_macevent.mpduDigest.mpducacheaddr;
                    int is_duplicate = 0;
                    
                    pthread_mutex_lock(&stats_lock);
                    
                    // 检查是否是重复包
                    for (uint64_t i = 0; i < seq_track_size; i++) {
                        if (received_seqs[i] == packet_seq) {
                            is_duplicate = 1;
                            break;
                        }
                    }
                    
                    if (!is_duplicate) {
                        // 不是重复包，添加到已接收序列数组
                        if (seq_track_size >= MAX_SEQ_TRACK) {
                            // 超出跟踪范围，重置跟踪数组
                            free(received_seqs);
                            received_seqs = NULL;
                            seq_track_size = 0;
                        }
                        
                        // 重新分配内存或初始化
                        if (received_seqs == NULL) {
                            received_seqs = (uint64_t *)malloc(MAX_SEQ_TRACK * sizeof(uint64_t));
                            if (received_seqs == NULL) {
                                fprintf(stderr, "[Node1接收线程] 内存分配失败\n");
                                pthread_mutex_unlock(&stats_lock);
                                continue;
                            }
                        }
                        
                        received_seqs[seq_track_size++] = packet_seq;
                        packets_received++;
                        
                        // 检查是否应该开始统计
                        if (!statistics_running) {
                            clock_gettime(CLOCK_MONOTONIC, &current_time);
                            double elapsed_sec = (current_time.tv_sec - start_time.tv_sec) +
                                              (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
                            
                            if (elapsed_sec >= 1.0) { // 第一秒后开始统计
                                statistics_running = 1;
                                statistics_start_time = current_time;
                                printf("[Node1接收线程] 开始统计收包...\n");
                            }
                        }
                        
                        // 检查是否在统计期间
                        if (statistics_running) {
                            clock_gettime(CLOCK_MONOTONIC, &current_time);
                            double statistics_elapsed = (current_time.tv_sec - statistics_start_time.tv_sec) +
                                                     (current_time.tv_nsec - statistics_start_time.tv_nsec) / 1e9;
                            
                            if (statistics_elapsed < TEST_DURATION) {
                                // 在统计期间，计数
                                packets_received_statistics++;
                            } else if (statistics_elapsed >= TEST_DURATION && statistics_running) {
                                // 统计时间结束
                                statistics_running = 0;
                                printf("[Node1接收线程] 统计结束，共统计 %llu 个数据包\n", (unsigned long long)packets_received_statistics);
                            }
                        }
                        
                        if (packets_received % 1000 == 0) {
                            printf("[Node1接收线程] 已收到 %llu 个数据包\n", (unsigned long long)packets_received);
                        }
                    }
                    
                    pthread_mutex_unlock(&stats_lock);
                }
            } else if (ret == 0) {
                if (packets_read == 0) {
                    consecutive_empty++;
                    if (consecutive_empty >= max_empty_count) {
                        consecutive_empty = 0;
                    }
                }
            } else {
                fprintf(stderr, "[Node1接收线程] Error: Failed to handle RX queue\n");
                break;
            }
        } while (ret == 1);

        usleep(100);
    }

    printf("[Node1接收线程] 接收完成，共收到 %llu 个数据包，其中统计 %llu 个数据包\n", 
           (unsigned long long)packets_received, (unsigned long long)packets_received_statistics);
    return NULL;
}

// 吞吐量统计结果结构体
typedef struct {
    double throughput;    // 吞吐量（Mbps）
    uint64_t total_bytes; // 总字节数
    double duration;      // 测试持续时间（秒）
    uint64_t packets_sent;   // 发送的数据包数
    uint64_t packets_received; // 收到的数据包数
    uint64_t packets_received_statistics; // 用于统计的收包数
} ThroughputStats;

// 计算吞吐量统计结果
ThroughputStats calculate_throughput_stats() {
    ThroughputStats stats;
    stats.packets_sent = packets_sent;
    stats.packets_received = packets_received;
    stats.packets_received_statistics = packets_received_statistics;
    stats.duration = TEST_DURATION;
    stats.total_bytes = packets_received_statistics * PACKET_SIZE;
    
    // 计算吞吐量（Mbps）
    if (stats.duration > 0) {
        stats.throughput = (stats.total_bytes * 8.0) / (stats.duration * 1000000.0);
    } else {
        stats.throughput = 0;
    }

    return stats;
}

// 重置统计数据
void reset_stats() {
    packets_sent = 0;
    packets_received = 0;
    packets_received_statistics = 0;
    running = 1;
    statistics_running = 0;
    
    // 重置序列号跟踪数组
    if (received_seqs != NULL) {
        free(received_seqs);
        received_seqs = NULL;
    }
    seq_track_size = 0;
}

int main() {
    RealEmu_Device *realemu_device = NULL;
    int ret;
    pthread_t node0_tx_thread, node1_rx_thread;
    FILE *csv_file = NULL;

    // 0. 初始化设备
    printf("=== 初始化 RealEmu 设备 ===\n");
    realemu_device = realemu_device_init("/dev/xdma0_h2c_0", "/dev/xdma0_c2h_0", "/dev/xdma0_user");
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Failed to initialize RealEmu device\n");
        return 1;
    }
    printf("设备初始化成功，node_num: %d\n\n", realemu_device->node_num);

    // 1. 配置拓扑：节点0和1之间的距离为8
    printf("=== 配置拓扑（Node0 <-> Node1，距离=8）===\n");
    ChannelCfg channel_cfg;

    // 首先设置所有节点对的距离为1023（默认值）
    for (int src = 0; src < MAX_NODES; src++) {
        for (int dst = 0; dst < MAX_NODES; dst++) {
            if (src != dst) {
                channel_cfg.srcPhyId = src;
                channel_cfg.dstPhyId = dst;
                channel_cfg.distance = DEFAULT_DISTANCE;
                send_topo_data(realemu_device, channel_cfg);
            }
        }
    }

    // 设置节点0和1之间的距离为8（双向）
    channel_cfg.srcPhyId = 0;
    channel_cfg.dstPhyId = 1;
    channel_cfg.distance = TEST_DISTANCE;
    send_topo_data(realemu_device, channel_cfg);

    channel_cfg.srcPhyId = 1;
    channel_cfg.dstPhyId = 0;
    channel_cfg.distance = TEST_DISTANCE;
    send_topo_data(realemu_device, channel_cfg);

    realemu_handle_tx_queue(realemu_device);
    printf("拓扑配置完成\n\n");

    // 打开CSV文件
    csv_file = fopen("throughput_results.csv", "w");
    if (csv_file) {
        printf("已打开CSV文件: throughput_results.csv\n");
    } else {
        fprintf(stderr, "警告: 无法打开CSV文件\n");
    }

    // 输出表头
    const char *header = "Rate,Throughput (Mbps),Packet Size (bytes),Distance (m),Total Bytes,Duration (s),Packets Sent,Packets Received\n";
    printf("%s", header);
    if (csv_file) {
        fprintf(csv_file, "%s", header);
        fflush(csv_file);
    }

    // 2. 循环测试不同的MCS速率
    for (size_t i = 0; i < MCS_COUNT; i++) {
        int mcs = mcs_rate_map[i].mcs;
        const char *rate_name = mcs_rate_map[i].rate_name;

        printf("\n=== 测试 %s (MCS=%d) ===\n", rate_name, mcs);
        
        // 重置统计数据
        reset_stats();

        // 设置节点0和1的SIFS值为50
        printf("设置节点0和1的SIFS值为50...\n");
        
        uint32_t sifs_value = 50;
        
        // 设置节点0的SIFS值
        int ret0 = realemu_write_reg(realemu_device, 0, "SIFS", sifs_value);
        if (ret0 != 0) {
            fprintf(stderr, "警告: 设置节点0的SIFS值失败\n");
        } else {
            printf("节点0的SIFS值设置为50\n");
        }
        
        // 设置节点1的SIFS值
        int ret1 = realemu_write_reg(realemu_device, 1, "SIFS", sifs_value);
        if (ret1 != 0) {
            fprintf(stderr, "警告: 设置节点1的SIFS值失败\n");
        } else {
            printf("节点1的SIFS值设置为50\n");
        }

        // 启动Node1接收线程
        ret = pthread_create(&node1_rx_thread, NULL, node1_rx_thread_func, realemu_device);
        if (ret != 0) {
            fprintf(stderr, "Error: Failed to create Node1 RX thread\n");
            goto cleanup;
        }

        // 等待一小段时间确保接收线程准备好
        usleep(100000);

        // 准备线程参数
        ThreadArgs args;
        args.realemu_device = realemu_device;
        args.mcs = mcs;

        // 启动Node0发送线程
        ret = pthread_create(&node0_tx_thread, NULL, node0_tx_thread_func, &args);
        if (ret != 0) {
            fprintf(stderr, "Error: Failed to create Node0 TX thread\n");
            goto cleanup;
        }

        // 3. 等待发送完成
        pthread_join(node0_tx_thread, NULL);
        printf("Node0发送完成\n");

        // 4. 等待接收完成
        printf("等待最多10秒...\n");
        
        // 创建一个等待线程
        pthread_t wait_thread;
        
        void *wait_func(void *arg) {
            sleep(10);
            running = 0;
            printf("[等待线程] 超时，强制退出\n");
            return NULL;
        }
        
        pthread_create(&wait_thread, NULL, wait_func, NULL);
        
        // 等待接收线程完成
        pthread_join(node1_rx_thread, NULL);
        printf("Node1接收完成\n");
        
        // 取消等待线程
        pthread_cancel(wait_thread);
        pthread_join(wait_thread, NULL);

        // 计算吞吐量统计
        ThroughputStats stats = calculate_throughput_stats();

        // 按照指定格式输出结果
        char result_line[256];
        snprintf(result_line, sizeof(result_line), "%s,%.6f,%d,%d,%llu,%.6f,%llu,%llu\n",
                 rate_name,
                 stats.throughput,
                 PACKET_SIZE,
                 TEST_DISTANCE,
                 (unsigned long long)stats.total_bytes,
                 stats.duration,
                 (unsigned long long)stats.packets_sent,
                 (unsigned long long)stats.packets_received);
        
        // 输出到终端
        printf("%s", result_line);
        
        // 输出到CSV文件
        if (csv_file) {
            fprintf(csv_file, "%s", result_line);
            fflush(csv_file);
        }
    }

    printf("\n=== 吞吐量测试完成 ===\n");

cleanup:
    // 关闭CSV文件
    if (csv_file) {
        fclose(csv_file);
        printf("CSV文件已关闭: throughput_results.csv\n");
    }

    // 释放序列号跟踪数组
    if (received_seqs != NULL) {
        free(received_seqs);
        received_seqs = NULL;
    }

    // 清理资源
    if (realemu_device != NULL) {
        if (realemu_device->xdma_h2c_fd >= 0) close(realemu_device->xdma_h2c_fd);
        if (realemu_device->xdma_c2h_fd >= 0) close(realemu_device->xdma_c2h_fd);
        if (realemu_device->user_reg_fd >= 0) close(realemu_device->user_reg_fd);

        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                for (int j = 0; j < REALEMU_QUEUE_DEPTH; j++) {
                    if (realemu_device->tx_queue[i]->data[j] != NULL) {
                        free(realemu_device->tx_queue[i]->data[j]);
                    }
                }
                pthread_mutex_destroy(&realemu_device->tx_queue[i]->lock);
                free(realemu_device->tx_queue[i]);
            }
        }

        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                for (int j = 0; j < REALEMU_QUEUE_DEPTH; j++) {
                    if (realemu_device->rx_queue[i]->data[j] != NULL) {
                        free(realemu_device->rx_queue[i]->data[j]);
                    }
                }
                pthread_mutex_destroy(&realemu_device->rx_queue[i]->lock);
                free(realemu_device->rx_queue[i]);
            }
        }

        free(realemu_device);
    }

    pthread_mutex_destroy(&stats_lock);

    return 0;
}
