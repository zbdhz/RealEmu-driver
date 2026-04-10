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

#include "realemu_hw.h"
#include "../tools/reg_rw.h"
#include "../tools/dma_with_device.h"
#include "../tools/dma_utils.h"

#define MAX_NODES 16
#define FORWARDING_NODES 8  // 节点0-7参与转发
#define DEFAULT_DISTANCE 1023
#define CHAIN_DISTANCE 8
#define MAX_QUEUE_SIZE 10000

// 数据包去重记录
typedef struct {
    uint64_t mpducacheaddr;
    int src_node;
    int received;
} PacketRecord;

// 发送队列项
typedef struct {
    MacEvent macevent;
    int src_node;
    int dst_node;
    uint64_t mpducacheaddr;
} SendQueueItem;

// 接收线程参数结构体
typedef struct {
    RealEmu_Device *realemu_device;
    volatile int *running;
    PacketRecord *packet_records;
    int max_records;
    pthread_mutex_t *record_lock;
    FILE *log_files[FORWARDING_NODES];
    pthread_mutex_t *log_lock;
    SendQueueItem *send_queue;
    int *queue_count;
    pthread_mutex_t *queue_lock;
    pthread_cond_t *queue_cond;
} RxThreadArgs;

// 发送线程参数结构体
typedef struct {
    RealEmu_Device *realemu_device;
    volatile int *running;
    int node_id;
    uint64_t *packet_seq;
    uint64_t total_packets;
    FILE *log_file;
    pthread_mutex_t *log_lock;
    SendQueueItem *send_queue;
    int *queue_count;
    pthread_mutex_t *queue_lock;
    pthread_cond_t *queue_cond;
} InitTxThreadArgs;

// 转发线程参数结构体
typedef struct {
    RealEmu_Device *realemu_device;
    volatile int *running;
    SendQueueItem *send_queue;
    int *queue_count;
    pthread_mutex_t *queue_lock;
    pthread_cond_t *queue_cond;
} ForwardTxThreadArgs;

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

// 检查数据包是否已接收过
int is_packet_received(PacketRecord *records, int max_records, uint64_t mpducacheaddr, int src_node, pthread_mutex_t *lock) {
    pthread_mutex_lock(lock);
    // 线性查找匹配的记录
    for (int i = 0; i < max_records; i++) {
        if (records[i].mpducacheaddr == mpducacheaddr && 
            records[i].src_node == src_node && 
            records[i].received) {
            pthread_mutex_unlock(lock);
            return 1;
        }
    }
    pthread_mutex_unlock(lock);
    return 0;
}

// 标记数据包已接收
void mark_packet_received(PacketRecord *records, int max_records, uint64_t mpducacheaddr, int src_node, pthread_mutex_t *lock) {
    pthread_mutex_lock(lock);
    // 查找是否已存在该数据包的记录
    int found = 0;
    for (int i = 0; i < max_records; i++) {
        if (records[i].mpducacheaddr == mpducacheaddr && 
            records[i].src_node == src_node) {
            // 找到已存在的记录，更新为已接收
            records[i].received = 1;
            found = 1;
            break;
        }
    }
    
    // 如果没有找到，找到一个未使用的记录位置
    if (!found) {
        for (int i = 0; i < max_records; i++) {
            if (!records[i].received) {
                records[i].mpducacheaddr = mpducacheaddr;
                records[i].src_node = src_node;
                records[i].received = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(lock);
}

// 接收线程函数
void *rx_thread_func(void *arg) {
    RxThreadArgs *args = (RxThreadArgs *)arg;
    RealEmu_Device *realemu_device = args->realemu_device;
    volatile int *running = args->running;
    PacketRecord *packet_records = args->packet_records;
    int max_records = args->max_records;
    pthread_mutex_t *record_lock = args->record_lock;
    FILE *log_files[FORWARDING_NODES];
    memcpy(log_files, args->log_files, sizeof(log_files));
    pthread_mutex_t *log_lock = args->log_lock;
    
    MacEvent rx_macevent;
    int consecutive_empty = 0;
    int max_empty_count = 100;
    
    printf("[接收线程] 启动接收...\n");
    
    while (*running) {
        // 从设备中更新rx队列
        // usleep(10000);
        int received_count = realemu_update_rx_queue(realemu_device);
        if (received_count < 0) {
            fprintf(stderr, "[接收线程] Error: Failed to update RX queue\n");
            break;
        }
        
        // 从rx队列中取出所有可用数据
        int ret;
        int packets_read = 0;
        
        do {
            ret = realemu_handle_rx_queue(realemu_device, &rx_macevent);
            if (ret == 1) {
                consecutive_empty = 0;
                packets_read++;
                
                int node_id = rx_macevent.dstMacId;
                
                // 检查是否是发给转发节点的数据包（0-7）
                if (node_id >= 0 && node_id < FORWARDING_NODES) {
                    uint64_t mpducacheaddr = rx_macevent.mpduDigest.mpducacheaddr;
                    
                    // 记录接收时间戳（在实际接收时）
                    uint64_t receive_timestamp = get_timestamp_us();
                    
                    // 检查是否已接收过（按源节点和序号判断）
                    if (!is_packet_received(packet_records, max_records, mpducacheaddr, rx_macevent.srcMacId, record_lock)) {
                        // 标记为已接收
                        mark_packet_received(packet_records, max_records, mpducacheaddr, rx_macevent.srcMacId, record_lock);
                        
                        // 写入接收日志
                        write_log(log_files[node_id], log_lock, "RECEIVE", receive_timestamp,
                                rx_macevent.mpduDigest.mpdulen, 
                                mpducacheaddr, 
                                rx_macevent.srcMacId, 
                                node_id);
                        
                        // printf("[接收线程] 收到数据包: src=%d dst=%d mpducacheaddr=0x%llX\n",
                        //        rx_macevent.srcMacId, node_id,
                        //        (unsigned long long)mpducacheaddr);
                        
                        // 如果节点1-7，需要转发
                        if (node_id >= 1 && node_id < FORWARDING_NODES) {
                            int next_hop = node_id + 1;
                            
                            // 准备转发的数据包
                            SendQueueItem item;
                            MacEvent tx_macevent;
                            memcpy(&tx_macevent, &rx_macevent, sizeof(MacEvent));
                            tx_macevent.srcMacId = node_id;
                            tx_macevent.dstMacId = next_hop;
                            tx_macevent.rfParam.power = 2000;  // 重新初始化功率值
                            
                            item.macevent = tx_macevent;
                            item.src_node = node_id;
                            item.dst_node = next_hop;
                            item.mpducacheaddr = mpducacheaddr;
                            
                            // 将转发数据包加入发送队列
                            pthread_mutex_lock(args->queue_lock);
                            if (*(args->queue_count) < MAX_QUEUE_SIZE) {
                                args->send_queue[*(args->queue_count)] = item;
                                (*(args->queue_count))++;
                                pthread_cond_signal(args->queue_cond);
                                
                                // 记录转发时间戳
                                uint64_t forward_timestamp = get_timestamp_us();
                                // 写入转发日志
                                write_log(log_files[node_id], log_lock, "FORWARD", forward_timestamp,
                                        tx_macevent.mpduDigest.mpdulen,
                                        mpducacheaddr,
                                        node_id,
                                        next_hop);
                                
                                // printf("[接收线程] 转发数据包到节点%d: mpducacheaddr=0x%llX\n",
                                //        next_hop, (unsigned long long)mpducacheaddr);
                            } else {
                                // printf("[接收线程] 发送队列已满，丢弃数据包\n");
                            }
                            pthread_mutex_unlock(args->queue_lock);
                        }
                    }
                }
            } else if (ret == 0) {
                if (packets_read == 0) {
                    consecutive_empty++;
                    if (consecutive_empty >= max_empty_count) {
                        printf("[接收线程] 连续 %d 次读不到数据，停止接收\n", 
                               max_empty_count);
                        break;
                    }
                }
            } else {
                fprintf(stderr, "[接收线程] Error: Failed to handle RX queue\n");
                break;
            }
        } while (ret == 1);
    }
    
    printf("[接收线程] 退出\n");
    return NULL;
}

// 初始发送线程函数（仅节点0使用，生成固定间隔的初始数据包并放入发送队列）
void *init_tx_thread_func(void *arg) {
    InitTxThreadArgs *args = (InitTxThreadArgs *)arg;
    RealEmu_Device *realemu_device = args->realemu_device;
    volatile int *running = args->running;
    int node_id = args->node_id;
    uint64_t *packet_seq = args->packet_seq;
    uint64_t total_packets = args->total_packets;
    FILE *log_file = args->log_file;
    pthread_mutex_t *log_lock = args->log_lock;
    SendQueueItem *send_queue = args->send_queue;
    int *queue_count = args->queue_count;
    pthread_mutex_t *queue_lock = args->queue_lock;
    pthread_cond_t *queue_cond = args->queue_cond;
    
    printf("[初始发送线程-节点%d] 启动发送...\n", node_id);
    write_log(log_file, log_lock, "TX_THREAD_START", get_timestamp_us(), 0, 0, node_id, 0);
    
    uint64_t packets_sent = 0;
    struct timespec last_send_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &last_send_time);
    
    while (*running && packets_sent < total_packets) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // 计算经过的时间（毫秒）
        double elapsed_ms = (current_time.tv_sec - last_send_time.tv_sec) * 1000.0 + 
                          (current_time.tv_nsec - last_send_time.tv_nsec) / 1000000.0;
        
        // 如果距离上次发送已经过了10ms，发送下一个数据包
        if (elapsed_ms >= 10.0) {
            // 生成数据包
            MacEvent tx_macevent;
            memset(&tx_macevent, 0, sizeof(MacEvent));
            tx_macevent.status = 1;
            tx_macevent.mpduDigest.mpducacheaddr = (*packet_seq)++;
            tx_macevent.mpduDigest.mpdulen = 1000;
            tx_macevent.mpduDigest.duration = 0;
            tx_macevent.mpduDigest.framesubtype = 0;
            tx_macevent.mpduDigest.frametype = 2;
            tx_macevent.rfParam.mcs = 7;
            tx_macevent.rfParam.power = 2000;
            tx_macevent.dstMacId = 1;  // 节点0发送给节点1
            tx_macevent.srcMacId = node_id;
            
            // 将初始数据包加入发送队列
            SendQueueItem item;
            item.macevent = tx_macevent;
            item.src_node = node_id;
            item.dst_node = 1;
            item.mpducacheaddr = tx_macevent.mpduDigest.mpducacheaddr;
            
            pthread_mutex_lock(queue_lock);
            if (*queue_count < MAX_QUEUE_SIZE) {
                send_queue[*queue_count] = item;
                (*queue_count)++;
                pthread_cond_signal(queue_cond);
                
                // 记录发送时间戳
                uint64_t send_timestamp = get_timestamp_us();
                // 写入发送日志
                write_log(log_file, log_lock, "SEND", send_timestamp,
                        tx_macevent.mpduDigest.mpdulen,
                        tx_macevent.mpduDigest.mpducacheaddr,
                        node_id,
                        tx_macevent.dstMacId);
                
                // printf("[初始发送线程-节点%d] 生成第 %llu 个数据包: dst=%d mpducacheaddr=0x%llX\n",
                //        node_id, (unsigned long long)(*packet_seq - 1), 
                //        tx_macevent.dstMacId,
                //        (unsigned long long)tx_macevent.mpduDigest.mpducacheaddr);
                
                packets_sent++;
                last_send_time = current_time;
            } else {
                // printf("[初始发送线程-节点%d] 发送队列已满，丢弃数据包\n", node_id);
            }
            pthread_mutex_unlock(queue_lock);
        } else {
            // 短暂休眠，避免忙等
            usleep(100);
        }
    }
    
    printf("[初始发送线程-节点%d] 发送完成，退出\n", node_id);
    write_log(log_file, log_lock, "TX_THREAD_STOP", get_timestamp_us(), 0, 0, node_id, 0);
    return NULL;
}

// 转发发送线程函数（处理转发队列中的数据包）
void *forward_tx_thread_func(void *arg) {
    ForwardTxThreadArgs *args = (ForwardTxThreadArgs *)arg;
    RealEmu_Device *realemu_device = args->realemu_device;
    volatile int *running = args->running;
    SendQueueItem *send_queue = args->send_queue;
    int *queue_count = args->queue_count;
    pthread_mutex_t *queue_lock = args->queue_lock;
    pthread_cond_t *queue_cond = args->queue_cond;
    
    printf("[转发发送线程] 启动处理转发队列...\n");
    
    while (*running || *queue_count > 0) {
        // 检查发送队列
        pthread_mutex_lock(queue_lock);
        
        // 如果队列为空，等待新的数据包
        while (*queue_count == 0 && *running) {
            pthread_cond_wait(queue_cond, queue_lock);
        }
        
        // 处理队列中的所有数据包
        while (*queue_count > 0) {
            // 取出队列中的第一个数据包
            SendQueueItem item = send_queue[0];
            
            // 移动队列中的其他数据包
            for (int i = 1; i < *queue_count; i++) {
                send_queue[i-1] = send_queue[i];
            }
            (*queue_count)--;
            
            pthread_mutex_unlock(queue_lock);
            
            // 发送数据包
            int ret = send_pkt_data(realemu_device, item.macevent);
            if (ret == 0) {
                int handle_ret = realemu_handle_tx_queue(realemu_device);
                if (handle_ret > 0) {
                    // 写入转发发送日志
                    // printf("[转发发送线程] 发送转发数据包: src=%d dst=%d mpducacheaddr=0x%llX\n",
                    //        item.src_node, item.dst_node, (unsigned long long)item.mpducacheaddr);
                }
            }
            
            pthread_mutex_lock(queue_lock);
        }
        
        pthread_mutex_unlock(queue_lock);
        
        // 短暂休眠，避免忙等
        // usleep(100);
    }
    
    printf("[转发发送线程] 退出\n");
    return NULL;
}

int main() {
    RealEmu_Device *realemu_device = NULL;
    int ret;
    pthread_t init_tx_thread, forward_tx_thread, rx_thread;
    InitTxThreadArgs init_tx_args;
    ForwardTxThreadArgs forward_tx_args;
    RxThreadArgs rx_args;
    volatile int running = 1;
    uint64_t packet_seq = 0;
    pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
    
    FILE *log_files[FORWARDING_NODES];
    char log_filename[256];
    
    PacketRecord packet_records[10000];  // 记录10000个数据包的去重信息
    int max_records = 10000;
    
    SendQueueItem send_queue[MAX_QUEUE_SIZE];  // 发送队列
    int queue_count = 0;
    
    // 初始化数据包记录
    for (int i = 0; i < max_records; i++) {
        packet_records[i].mpducacheaddr = 0;
        packet_records[i].src_node = -1;
        packet_records[i].received = 0;
    }

    // 0. 初始化设备
    printf("=== 初始化 RealEmu 设备 ===\n");
    realemu_device = realemu_device_init("/dev/xdma0_h2c_0", "/dev/xdma0_c2h_0", "/dev/xdma0_user");
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Failed to initialize RealEmu device\n");
        return 1;
    }
    printf("设备初始化成功，node_num: %d\n\n", realemu_device->node_num);

    // 1. 配置多跳拓扑（节点0-7链式连接，距离=8，其他距离=1023）
    printf("=== 配置多跳拓扑 ===\n");
    ChannelCfg channel_cfg;
    int topo_sent_count = 0;
    
    // 首先设置所有节点对的距离为1023（默认值）
    for (int src = 0; src < MAX_NODES; src++) {
        for (int dst = 0; dst < MAX_NODES; dst++) {
            if (src != dst) {
                channel_cfg.srcPhyId = src;
                channel_cfg.dstPhyId = dst;
                channel_cfg.distance = DEFAULT_DISTANCE;
                
                ret = send_topo_data(realemu_device, channel_cfg);
                if (ret == 0) {
                    topo_sent_count++;
                }
            }
        }
    }
    
    // 设置节点0-7之间的链式连接，距离为8
    for (int i = 0; i < FORWARDING_NODES - 1; i++) {
        channel_cfg.srcPhyId = i;
        channel_cfg.dstPhyId = i + 1;
        channel_cfg.distance = CHAIN_DISTANCE;
        
        ret = send_topo_data(realemu_device, channel_cfg);
        if (ret == 0) {
            topo_sent_count++;
        }
    }
    
    printf("成功配置 %d 个拓扑关系\n", topo_sent_count);
    
    // 发送拓扑数据到设备
    int topo_handle_count = realemu_handle_tx_queue(realemu_device);
    if (topo_handle_count < 0) {
        fprintf(stderr, "Error: Failed to handle TX queue for topo data\n");
        goto cleanup;
    }
    printf("成功发送 %d 个拓扑配置到设备\n\n", topo_handle_count);

    // 2. 打开日志文件
    printf("=== 打开日志文件 ===\n");
    for (int i = 0; i < FORWARDING_NODES; i++) {
        snprintf(log_filename, sizeof(log_filename), "node_%d.log", i);
        log_files[i] = fopen(log_filename, "w");
        if (log_files[i] == NULL) {
            fprintf(stderr, "Error: Failed to open log file %s\n", log_filename);
            goto cleanup;
        }
        printf("打开日志文件: %s\n", log_filename);
    }
    printf("\n");

    // 3. 启动接收线程（只有一个）
    printf("=== 启动接收线程 ===\n");
    rx_args.realemu_device = realemu_device;
    rx_args.running = &running;
    rx_args.packet_records = packet_records;
    rx_args.max_records = max_records;
    rx_args.record_lock = &record_lock;
    memcpy(rx_args.log_files, log_files, sizeof(log_files));
    rx_args.log_lock = &log_lock;
    rx_args.send_queue = send_queue;
    rx_args.queue_count = &queue_count;
    rx_args.queue_lock = &queue_lock;
    rx_args.queue_cond = &queue_cond;
    
    ret = pthread_create(&rx_thread, NULL, rx_thread_func, &rx_args);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to create RX thread\n");
        goto cleanup;
    }
    printf("接收线程已启动\n\n");
    
    // 4. 启动转发发送线程
    printf("=== 启动转发发送线程 ===\n");
    forward_tx_args.realemu_device = realemu_device;
    forward_tx_args.running = &running;
    forward_tx_args.send_queue = send_queue;
    forward_tx_args.queue_count = &queue_count;
    forward_tx_args.queue_lock = &queue_lock;
    forward_tx_args.queue_cond = &queue_cond;
    
    ret = pthread_create(&forward_tx_thread, NULL, forward_tx_thread_func, &forward_tx_args);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to create forward TX thread\n");
        goto cleanup;
    }
    printf("转发发送线程已启动\n\n");
    
    // 等待一小段时间确保线程开始运行
    usleep(100000);

    // 5. 启动初始发送线程（仅节点0）
    printf("=== 启动初始发送线程 ===\n");
    init_tx_args.realemu_device = realemu_device;
    init_tx_args.running = &running;
    init_tx_args.node_id = 0;
    init_tx_args.packet_seq = &packet_seq;
    init_tx_args.total_packets = 1000;  // 发送1000个数据包
    init_tx_args.log_file = log_files[0];
    init_tx_args.log_lock = &log_lock;
    init_tx_args.send_queue = send_queue;
    init_tx_args.queue_count = &queue_count;
    init_tx_args.queue_lock = &queue_lock;
    init_tx_args.queue_cond = &queue_cond;
    
    ret = pthread_create(&init_tx_thread, NULL, init_tx_thread_func, &init_tx_args);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to create initial TX thread for node 0\n");
        goto cleanup;
    }
    printf("初始发送线程-节点0 已启动（发送1000个数据包）\n\n");

    // 6. 等待发送和接收完成
    printf("=== 多跳模型运行中 ===\n");
    printf("节点0 -> 节点1 -> 节点2 -> ... -> 节点7\n");
    printf("发送数量: 1000 个数据包\n\n");
    
    // 7. 等待初始发送线程完成
    printf("\n=== 等待初始发送线程完成 ===\n");
    pthread_join(init_tx_thread, NULL);
    printf("初始发送线程-节点0 已完成\n");
    
    // 等待一段时间，让所有数据包处理完毕（包括上一次实验的残留数据包）
    printf("\n=== 等待所有数据包处理完毕 ===\n");
    printf("等待30秒，确保所有数据包都能完成转发...\n");
    for (int i = 0; i < 10; i++) {
        printf("等待中... %d/10\r", i+1);
        fflush(stdout);
        sleep(1);
    }
    printf("\n");
    
    // 8. 等待转发发送线程完成（处理完所有转发数据包）
    printf("\n=== 等待转发发送线程完成 ===\n");
    // 设置running为false，通知转发发送线程退出
    running = 0;
    // 唤醒转发发送线程
    pthread_cond_signal(&queue_cond);
    pthread_join(forward_tx_thread, NULL);
    printf("转发发送线程已完成\n");
    
    // 9. 等待接收线程完成
    printf("\n=== 等待接收线程完成 ===\n");
    pthread_join(rx_thread, NULL);
    printf("接收线程已停止\n");
    
    printf("\n=== 运行结束 ===\n");
    printf("总共发送 %llu 个数据包\n", (unsigned long long)packet_seq);

cleanup:
    // 关闭日志文件
    printf("\n=== 关闭日志文件 ===\n");
    for (int i = 0; i < FORWARDING_NODES; i++) {
        if (log_files[i] != NULL) {
            fclose(log_files[i]);
            printf("关闭日志文件: node_%d.log\n", i);
        }
    }
    
    // 清理资源
    printf("\n=== 清理资源 ===\n");
    if (realemu_device != NULL) {
        if (realemu_device->xdma_h2c_fd >= 0) {
            close(realemu_device->xdma_h2c_fd);
        }
        if (realemu_device->xdma_c2h_fd >= 0) {
            close(realemu_device->xdma_c2h_fd);
        }
        if (realemu_device->user_reg_fd >= 0) {
            close(realemu_device->user_reg_fd);
        }
        
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
        printf("资源清理完成\n");
    }
    
    pthread_mutex_destroy(&log_lock);
    pthread_mutex_destroy(&record_lock);
    pthread_mutex_destroy(&queue_lock);
    pthread_cond_destroy(&queue_cond);

    return 0;
}
