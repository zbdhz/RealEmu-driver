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
#define CHAIN_DISTANCE 8
#define MAX_QUEUE_SIZE 10000
#define TOTAL_PACKETS 1000
#define PACKET_INTERVAL_MS 10.0

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

// RTT记录结构体
typedef struct {
    uint64_t seq;
    uint64_t send_time;      // 节点0发送时间
    uint64_t recv_time;      // 节点0收到回包时间
    int valid;               // 是否有效
} RttRecord;

// 发送队列项
typedef struct {
    MacEvent macevent;
    int src_node;
    int dst_node;
    uint64_t mpducacheaddr;
} SendQueueItem;

// 线程参数结构体
typedef struct {
    RealEmu_Device *realemu_device;
    int mcs;
} ThreadArgs;

// 全局变量
RttRecord rtt_records[TOTAL_PACKETS];
pthread_mutex_t rtt_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int packets_received_back = 0;
volatile int running = 1;

// 获取当前时间戳（微秒）
uint64_t get_timestamp_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
}

// 写入日志
void write_log(FILE *log_file, pthread_mutex_t *log_lock, const char *action,
              uint64_t timestamp, uint64_t seq, uint64_t mpducacheaddr, int src, int dst) {
    if (log_file == NULL) return;

    pthread_mutex_lock(log_lock);
    fprintf(log_file, "[%llu] %s seq=%llu mpducacheaddr=0x%llX src=%d dst=%d\n",
            (unsigned long long)timestamp,
            action,
            (unsigned long long)seq,
            (unsigned long long)mpducacheaddr,
            src, dst);
    fflush(log_file);
    pthread_mutex_unlock(log_lock);
}

// 节点0发送线程：发送数据包给节点1
void *node0_tx_thread_func(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    RealEmu_Device *realemu_device = args->realemu_device;
    int mcs = args->mcs;
    uint64_t packet_seq = 0;
    uint64_t packets_sent = 0;
    struct timespec last_send_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &last_send_time);

    printf("[Node0发送线程] 启动，发送 %d 个数据包给 Node1 (MCS=%d)...\n", TOTAL_PACKETS, mcs);

    while (running && packets_sent < TOTAL_PACKETS) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        double elapsed_ms = (current_time.tv_sec - last_send_time.tv_sec) * 1000.0 +
                          (current_time.tv_nsec - last_send_time.tv_nsec) / 1000000.0;

        if (elapsed_ms >= PACKET_INTERVAL_MS) {
            // 生成数据包
            MacEvent tx_macevent;
            memset(&tx_macevent, 0, sizeof(MacEvent));
            tx_macevent.status = 1;
            tx_macevent.mpduDigest.mpducacheaddr = packet_seq;
            tx_macevent.mpduDigest.mpdulen = 1200;
            tx_macevent.mpduDigest.duration = 0;
            tx_macevent.mpduDigest.framesubtype = 0;
            tx_macevent.mpduDigest.frametype = 2;
            tx_macevent.rfParam.mcs = mcs;
            tx_macevent.rfParam.power = 2000;
            tx_macevent.dstMacId = 1;  // 发送给节点1
            tx_macevent.srcMacId = 0;

            // 记录发送时间
            uint64_t send_time = get_timestamp_us();

            pthread_mutex_lock(&rtt_lock);
            rtt_records[packet_seq].seq = packet_seq;
            rtt_records[packet_seq].send_time = send_time;
            rtt_records[packet_seq].valid = 0;  // 标记为未收到回包
            pthread_mutex_unlock(&rtt_lock);

            // 发送数据包
            int ret = send_pkt_data(realemu_device, tx_macevent);
            if (ret == 0) {
                realemu_handle_tx_queue(realemu_device);
                packets_sent++;
                last_send_time = current_time;

                if (packets_sent % 100 == 0) {
                    printf("[Node0发送线程] 已发送 %llu/%d 个数据包\n",
                           (unsigned long long)packets_sent, TOTAL_PACKETS);
                }
            }

            packet_seq++;
        } else {
            usleep(100);
        }
    }

    printf("[Node0发送线程] 发送完成，共发送 %llu 个数据包\n", (unsigned long long)packets_sent);
    
    // 发送完成，等待一段时间让所有数据包处理完毕
    printf("[Node0发送线程] 等待3秒让所有数据包处理完毕...\n");
    sleep(3);
    
    // 设置running为false，通知其他线程退出
    running = 0;
    
    return NULL;
}

// 节点0接收线程：接收节点1回发的数据包
void *node0_rx_thread_func(void *arg) {
    RealEmu_Device *realemu_device = (RealEmu_Device *)arg;
    MacEvent rx_macevent;
    int consecutive_empty = 0;
    int max_empty_count = 500;  // 增加等待时间

    printf("[Node0接收线程] 启动，等待接收 Node1 回发的数据包...\n");

    while (running && packets_received_back < TOTAL_PACKETS) {
        int received_count = realemu_update_rx_queue(realemu_device);
        if (received_count < 0) {
            // 遇到错误，继续运行但记录错误
            fprintf(stderr, "[Node0接收线程] Error: Failed to update RX queue\n");
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

                // 检查是否是发给节点0的数据包
                if (rx_macevent.dstMacId == 0 && rx_macevent.srcMacId == 1) {
                    uint64_t seq = rx_macevent.mpduDigest.mpducacheaddr;
                    uint64_t recv_time = get_timestamp_us();

                    if (seq < TOTAL_PACKETS) {
                        pthread_mutex_lock(&rtt_lock);
                        if (rtt_records[seq].valid == 0) {
                            rtt_records[seq].recv_time = recv_time;
                            rtt_records[seq].valid = 1;
                            packets_received_back++;

                            if (packets_received_back % 100 == 0) {
                                printf("[Node0接收线程] 已收到 %d/%d 个回包\n",
                                       packets_received_back, TOTAL_PACKETS);
                            }
                        }
                        pthread_mutex_unlock(&rtt_lock);
                    }
                }
            } else if (ret == 0) {
                if (packets_read == 0) {
                    consecutive_empty++;
                    if (consecutive_empty >= max_empty_count) {
                        printf("[Node0接收线程] 连续 %d 次读不到数据\n", max_empty_count);
                        // 不退出，继续等待
                        consecutive_empty = 0;
                    }
                }
            } else {
                fprintf(stderr, "[Node0接收线程] Error: Failed to handle RX queue\n");
                break;
            }
        } while (ret == 1);

        usleep(100);
    }

    printf("[Node0接收线程] 接收完成，共收到 %d 个回包\n", packets_received_back);
    return NULL;
}

// 节点1接收和回发线程
void *node1_rx_tx_thread_func(void *arg) {
    RealEmu_Device *realemu_device = (RealEmu_Device *)arg;
    MacEvent rx_macevent;
    int consecutive_empty = 0;
    int max_empty_count = 500;
    int packets_forwarded = 0;

    printf("[Node1接收回发线程] 启动，接收 Node0 的数据包并立即回发...\n");

    while (running && packets_forwarded < TOTAL_PACKETS) {
        int received_count = realemu_update_rx_queue(realemu_device);
        if (received_count < 0) {
            // 遇到错误，继续运行但记录错误
            fprintf(stderr, "[Node1接收回发线程] Error: Failed to update RX queue\n");
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
                    uint64_t seq = rx_macevent.mpduDigest.mpducacheaddr;

                    // 立即回发给节点0
                    MacEvent tx_macevent;
                    memcpy(&tx_macevent, &rx_macevent, sizeof(MacEvent));
                    tx_macevent.srcMacId = 1;
                    tx_macevent.dstMacId = 0;
                    tx_macevent.rfParam.power = 2000;
                    tx_macevent.mpduDigest.mpducacheaddr = seq;  // 保持相同的序列号

                    // 发送回包
                    int send_ret = send_pkt_data(realemu_device, tx_macevent);
                    if (send_ret == 0) {
                        realemu_handle_tx_queue(realemu_device);
                        packets_forwarded++;

                        if (packets_forwarded % 100 == 0) {
                            printf("[Node1接收回发线程] 已回发 %d/%d 个数据包\n",
                                   packets_forwarded, TOTAL_PACKETS);
                        }
                    }
                }
            } else if (ret == 0) {
                if (packets_read == 0) {
                    consecutive_empty++;
                    if (consecutive_empty >= max_empty_count) {
                        printf("[Node1接收回发线程] 连续 %d 次读不到数据\n", max_empty_count);
                        consecutive_empty = 0;
                    }
                }
            } else {
                fprintf(stderr, "[Node1接收回发线程] Error: Failed to handle RX queue\n");
                break;
            }
        } while (ret == 1);

        usleep(100);
    }

    printf("[Node1接收回发线程] 完成，共回发 %d 个数据包\n", packets_forwarded);
    return NULL;
}

// RTT统计结果结构体
typedef struct {
    double min_rtt;    // 最小RTT（毫秒）
    double avg_rtt;    // 平均RTT（毫秒）
    double max_rtt;    // 最大RTT（毫秒）
    double std_dev;    // 标准差（毫秒）
    int packets_sent;  // 发送的数据包数
    int packets_received;  // 收到的数据包数
} RttStats;

// 计算RTT统计结果
RttStats calculate_rtt_stats() {
    RttStats stats;
    stats.min_rtt = 999999999;
    stats.max_rtt = 0;
    stats.avg_rtt = 0;
    stats.std_dev = 0;
    stats.packets_sent = TOTAL_PACKETS;
    stats.packets_received = 0;

    int valid_count = 0;
    double total_rtt = 0;

    for (int i = 0; i < TOTAL_PACKETS; i++) {
        if (rtt_records[i].valid) {
            double rtt = (double)(rtt_records[i].recv_time - rtt_records[i].send_time) / 1000.0; // 转换为毫秒
            total_rtt += rtt;
            if (rtt < stats.min_rtt) stats.min_rtt = rtt;
            if (rtt > stats.max_rtt) stats.max_rtt = rtt;
            valid_count++;
        }
    }

    stats.packets_received = valid_count;

    if (valid_count > 0) {
        stats.avg_rtt = total_rtt / valid_count;

        // 计算标准差
        double variance = 0;
        for (int i = 0; i < TOTAL_PACKETS; i++) {
            if (rtt_records[i].valid) {
                double rtt = (double)(rtt_records[i].recv_time - rtt_records[i].send_time) / 1000.0;
                variance += (rtt - stats.avg_rtt) * (rtt - stats.avg_rtt);
            }
        }
        stats.std_dev = sqrt(variance / valid_count);
    } else {
        stats.min_rtt = 0;
        stats.avg_rtt = 0;
        stats.max_rtt = 0;
        stats.std_dev = 0;
    }

    return stats;
}

// 重置RTT记录
void reset_rtt_records() {
    for (int i = 0; i < TOTAL_PACKETS; i++) {
        rtt_records[i].seq = i;
        rtt_records[i].valid = 0;
    }
    packets_received_back = 0;
    running = 1;
}

int main() {
    RealEmu_Device *realemu_device = NULL;
    int ret;
    pthread_t node0_tx_thread, node0_rx_thread, node1_rx_tx_thread;
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
    channel_cfg.distance = CHAIN_DISTANCE;
    send_topo_data(realemu_device, channel_cfg);

    channel_cfg.srcPhyId = 1;
    channel_cfg.dstPhyId = 0;
    channel_cfg.distance = CHAIN_DISTANCE;
    send_topo_data(realemu_device, channel_cfg);

    realemu_handle_tx_queue(realemu_device);
    printf("拓扑配置完成\n\n");

    // 打开CSV文件
    csv_file = fopen("rtt_results.csv", "w");
    if (csv_file) {
        printf("已打开CSV文件: rtt_results.csv\n");
    } else {
        fprintf(stderr, "警告: 无法打开CSV文件\n");
    }

    // 输出表头
    const char *header = "Rate,Min RTT (ms),Avg RTT (ms),Max RTT (ms),Std Dev RTT (ms),Packets Sent,Packets Received\n";
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
        
        // 重置RTT记录
        reset_rtt_records();

        // 启动Node0接收线程
        ret = pthread_create(&node0_rx_thread, NULL, node0_rx_thread_func, realemu_device);
        if (ret != 0) {
            fprintf(stderr, "Error: Failed to create Node0 RX thread\n");
            goto cleanup;
        }

        // 启动Node1接收回发线程
        ret = pthread_create(&node1_rx_tx_thread, NULL, node1_rx_tx_thread_func, realemu_device);
        if (ret != 0) {
            fprintf(stderr, "Error: Failed to create Node1 RX/TX thread\n");
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
        pthread_join(node0_rx_thread, NULL);
        printf("Node0接收完成\n");

        // 等待Node1处理完成
        pthread_join(node1_rx_tx_thread, NULL);
        printf("Node1处理完成\n");
        
        // 取消等待线程
        pthread_cancel(wait_thread);
        pthread_join(wait_thread, NULL);

        // 计算RTT统计
        RttStats stats = calculate_rtt_stats();

        // 按照指定格式输出结果
        char result_line[256];
        snprintf(result_line, sizeof(result_line), "%s,%.6f,%.6f,%.6f,%.6f,%d,%d\n",
                 rate_name,
                 stats.min_rtt,
                 stats.avg_rtt,
                 stats.max_rtt,
                 stats.std_dev,
                 stats.packets_sent,
                 stats.packets_received);
        
        // 输出到终端
        printf("%s", result_line);
        
        // 输出到CSV文件
        if (csv_file) {
            fprintf(csv_file, "%s", result_line);
            fflush(csv_file);
        }
    }

    printf("\n=== RTT测试完成 ===\n");

cleanup:
    // 关闭CSV文件
    if (csv_file) {
        fclose(csv_file);
        printf("CSV文件已关闭: rtt_results.csv\n");
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

    pthread_mutex_destroy(&rtt_lock);

    return 0;
}
