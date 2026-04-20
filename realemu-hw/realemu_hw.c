#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "realemu_hw.h"
#include "../tools/reg_rw.h"
#include "../tools/dma_with_device.h"
#include "../tools/dma_utils.h"

static void* rx_polling_thread(void *arg);
static void* tx_flush_thread(void *arg);

// 按照字节设置缓冲区中指定位置的位
void set_bit(uint8_t* buffer, size_t bit_pos, uint8_t value) {
    
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
void direct_reverse_cfg_bridge_topo_to_buffer(const CfgBridge_Topo* data, uint8_t* buffer) {
    size_t bit_pos = 0;
    // 从最低位开始反向设置
    set_bits(buffer, bit_pos, 7, data->bridgeTag.notUsed);
    bit_pos += 7;
    set_bit(buffer, bit_pos++, data->bridgeTag.control);
    set_bits(buffer, bit_pos, 10, data->channelCfg.distance);
    bit_pos += 10;    
    set_bits(buffer, bit_pos, 10, data->channelCfg.dstPhyId);
    bit_pos += 10;
    set_bits(buffer, bit_pos, 10, data->channelCfg.srcPhyId);
    bit_pos += 10;
}

void direct_reverse_cfg_bridge_per_to_buffer(const CfgBridge_Per* data, uint8_t* buffer){
    size_t bit_pos = 0;
    // 从最低位开始反向设置
    set_bits(buffer, bit_pos, 7, data->bridgeTag.notUsed);
    bit_pos += 7;
    set_bit(buffer, bit_pos++, data->bridgeTag.control);
    set_bits(buffer, bit_pos, 16, data->perCfg.perOut);
    bit_pos += 16;
    set_bits(buffer, bit_pos, 14, data->perCfg.perIn);
}

void buffer_to_mac_event(const uint8_t* buffer, MacEvent* data) {
    size_t bit_pos = 0;

    // 暂未在返回的数据包中添加控制帧，后续可以添加，则需更新转换函数
    // // 解析 notUsed 字段 (1位)
    // data->bridgeTag.notUsed = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    // bit_pos += 1;

    // // 解析 control 字段 (1位)
    // data->bridgeTag.control = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    // bit_pos += 1;

    // 解析 status 字段 (1位)
    data->status = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    bit_pos += 1;

    // 解析 mpducacheaddr 字段 (64位)
    uint64_t mpducacheaddr = 0;
    for (int i = 0; i < 64; i++) {
        mpducacheaddr |= ((uint64_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->mpduDigest.mpducacheaddr = mpducacheaddr;
    bit_pos += 64;

    // 解析 mpdulen 字段 (16位)
    uint16_t mpdulen = 0;
    for (int i = 0; i < 16; i++) {
        mpdulen |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->mpduDigest.mpdulen = mpdulen;
    bit_pos += 16;

    // 解析 duration 字段 (16位)
    uint16_t duration = 0;
    for (int i = 0; i < 16; i++) {
        duration |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->mpduDigest.duration = duration;
    bit_pos += 16;

    // 解析 framesubtype 字段 (4位)
    uint16_t framesubtype = 0;
    for (int i = 0; i < 4; i++) {
        framesubtype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->mpduDigest.framesubtype = framesubtype;
    bit_pos += 4;

    // 解析 frametype 字段 (2位)
    uint16_t frametype = 0;
    for (int i = 0; i < 2; i++) {
        frametype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->mpduDigest.frametype = frametype;
    bit_pos += 2;

    // 解析 mcs 字段 (4位)
    uint16_t mcs = 0;
    for (int i = 0; i < 4; i++) {
        mcs |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->rfParam.mcs = mcs;
    bit_pos += 4;

    // 解析 power 字段 (12位)
    uint16_t power = 0;
    for (int i = 0; i < 12; i++) {
        power |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->rfParam.power = power;
    bit_pos += 12;

    // 解析 dstMacId 字段 (10位)
    uint16_t dstMacId = 0;
    for (int i = 0; i < 10; i++) {
        dstMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->dstMacId = dstMacId;
    bit_pos += 10;

    // 解析 srcMacId 字段 (10位)
    uint16_t srcMacId = 0;
    for (int i = 0; i < 10; i++) {
        srcMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->srcMacId = srcMacId;
    bit_pos += 10;
}

static int realemu_init_tx_queue(RealEmu_Device* realemu_device, uint16_t qid) {
    if (realemu_device == NULL || qid >= REALEMU_TX_QUEUES) {
        return -1;
    }

    RealEmu_Tx_Queue *tx_queue = (RealEmu_Tx_Queue *)malloc(sizeof(RealEmu_Tx_Queue));
    if (tx_queue == NULL) {
        return -1;
    }

    memset(tx_queue, 0, sizeof(RealEmu_Tx_Queue));

    for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
        tx_queue->data[i] = (RealEmu_Queue_Data *)malloc(sizeof(RealEmu_Queue_Data));
        if (tx_queue->data[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(tx_queue->data[j]);
            }
            free(tx_queue);
            return -1;
        }
        memset(tx_queue->data[i], 0, sizeof(RealEmu_Queue_Data));
    }

    tx_queue->qid = qid;
    tx_queue->head = 0;
    tx_queue->tail = 0;
    tx_queue->count = 0;
    tx_queue->state = 0;

    if (pthread_mutex_init(&tx_queue->lock, NULL) != 0) {
        for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
            free(tx_queue->data[i]);
        }
        free(tx_queue);
        return -1;
    }

    realemu_device->tx_queue[qid] = tx_queue;
    return 0;
}

static int realemu_init_rx_queue(RealEmu_Device* realemu_device, uint16_t qid) {
    if (realemu_device == NULL || qid >= REALEMU_RX_QUEUES) {
        return -1;
    }

    RealEmu_Rx_Queue *rx_queue = (RealEmu_Rx_Queue *)malloc(sizeof(RealEmu_Rx_Queue));
    if (rx_queue == NULL) {
        return -1;
    }

    memset(rx_queue, 0, sizeof(RealEmu_Rx_Queue));

    for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
        rx_queue->data[i] = (RealEmu_Queue_Data *)malloc(sizeof(RealEmu_Queue_Data));
        if (rx_queue->data[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(rx_queue->data[j]);
            }
            free(rx_queue);
            return -1;
        }
        memset(rx_queue->data[i], 0, sizeof(RealEmu_Queue_Data));
    }

    rx_queue->qid = qid;
    rx_queue->head = 0;
    rx_queue->tail = 0;
    rx_queue->count = 0;
    rx_queue->state = 0;

    if (pthread_mutex_init(&rx_queue->lock, NULL) != 0) {
        for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
            free(rx_queue->data[i]);
        }
        free(rx_queue);
        return -1;
    }

    realemu_device->rx_queue[qid] = rx_queue;
    return 0;
}

static void realemu_tx_queue_clean(RealEmu_Tx_Queue *q) {
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->lock);

    for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
        if (q->data[i] != NULL) {
            free(q->data[i]);
            q->data[i] = NULL;
        }
    }

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->state = 0;

    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
}

static void realemu_rx_queue_clean(RealEmu_Rx_Queue *q) {
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->lock);

    for (int i = 0; i < REALEMU_QUEUE_DEPTH; i++) {
        if (q->data[i] != NULL) {
            free(q->data[i]);
            q->data[i] = NULL;
        }
    }

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->state = 0;

    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
}

static uint32_t realemu_get_node_base_addr(uint32_t node_id) {
    if (node_id >= REALEMU_NODE_COUNT) {
        return 0xFFFFFFFF;
    }
    return REALEMU_NODE_BASE_ADDR + (node_id * REALEMU_NODE_SIZE);
}

static uint32_t realemu_get_mac_reg_addr(uint32_t node_id, uint32_t reg_offset) {
    uint32_t node_base = realemu_get_node_base_addr(node_id);
    if (node_base == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    return node_base + REALEMU_MAC_OFFSET + reg_offset;
}

static uint32_t realemu_get_phy_reg_addr(uint32_t node_id, uint32_t reg_offset) {
    uint32_t node_base = realemu_get_node_base_addr(node_id);
    if (node_base == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    return node_base + REALEMU_PHY_OFFSET + reg_offset;
}

static const RegisterInfo* realemu_find_mac_reg_by_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    
    for (size_t i = 0; i < sizeof(mac_reg_table) / sizeof(RegisterInfo); i++) {
        if (strcmp(mac_reg_table[i].name, name) == 0) {
            return &mac_reg_table[i];
        }
    }
    return NULL;
}

static const RegisterInfo* realemu_find_phy_reg_by_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    
    for (size_t i = 0; i < sizeof(phy_reg_table) / sizeof(RegisterInfo); i++) {
        if (strcmp(phy_reg_table[i].name, name) == 0) {
            return &phy_reg_table[i];
        }
    }
    return NULL;
}

int realemu_write_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t value) {
    if (realemu_device == NULL || reg_name == NULL) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }
    
    if (realemu_device->user_reg_fd < 0) {
        fprintf(stderr, "Error: Invalid file descriptor\n");
        return -1;
    }
    
    if (node_id >= REALEMU_NODE_COUNT) {
        fprintf(stderr, "Error: Invalid node_id %u (max: %u)\n", node_id, REALEMU_NODE_COUNT - 1);
        return -1;
    }
    
    const RegisterInfo *mac_reg = realemu_find_mac_reg_by_name(reg_name);
    const RegisterInfo *phy_reg = realemu_find_phy_reg_by_name(reg_name);
    
    uint32_t addr;
    
    if (mac_reg != NULL) {
        if ((mac_reg->access & REG_ACCESS_WO) == 0) {
            fprintf(stderr, "Warning: MAC register '%s' is not writable\n", reg_name);
        }
        addr = realemu_get_mac_reg_addr(node_id, mac_reg->offset);
    } else if (phy_reg != NULL) {
        if ((phy_reg->access & REG_ACCESS_WO) == 0) {
            fprintf(stderr, "Warning: PHY register '%s' is not writable\n", reg_name);
        }
        addr = realemu_get_phy_reg_addr(node_id, phy_reg->offset);
    } else {
        fprintf(stderr, "Error: Register '%s' not found\n", reg_name);
        return -1;
    }
    
    if (addr == 0xFFFFFFFF) {
        fprintf(stderr, "Error: Failed to calculate register address\n");
        return -1;
    }
    
    if (reg_write(realemu_device->user_reg_fd, addr, value) != 0) {
        fprintf(stderr, "Error: Failed to write register '%s' at node %u (addr: 0x%08X)\n", 
                reg_name, node_id, addr);
        return -1;
    }
    
    return 0;
}

int realemu_read_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t *value) {
    if (realemu_device == NULL || reg_name == NULL || value == NULL) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }
    
    if (realemu_device->user_reg_fd < 0) {
        fprintf(stderr, "Error: Invalid file descriptor\n");
        return -1;
    }
    
    if (node_id >= REALEMU_NODE_COUNT) {
        fprintf(stderr, "Error: Invalid node_id %u (max: %u)\n", node_id, REALEMU_NODE_COUNT - 1);
        return -1;
    }
    
    const RegisterInfo *mac_reg = realemu_find_mac_reg_by_name(reg_name);
    const RegisterInfo *phy_reg = realemu_find_phy_reg_by_name(reg_name);
    
    uint32_t addr;
    
    if (mac_reg != NULL) {
        if ((mac_reg->access & REG_ACCESS_RO) == 0) {
            fprintf(stderr, "Warning: MAC register '%s' is not readable\n", reg_name);
        }
        addr = realemu_get_mac_reg_addr(node_id, mac_reg->offset);
    } else if (phy_reg != NULL) {
        if ((phy_reg->access & REG_ACCESS_RO) == 0) {
            fprintf(stderr, "Warning: PHY register '%s' is not readable\n", reg_name);
        }
        addr = realemu_get_phy_reg_addr(node_id, phy_reg->offset);
    } else {
        fprintf(stderr, "Error: Register '%s' not found\n", reg_name);
        return -1;
    }
    
    if (addr == 0xFFFFFFFF) {
        fprintf(stderr, "Error: Failed to calculate register address\n");
        return -1;
    }
    
    if (reg_read(realemu_device->user_reg_fd, addr, value) != 0) {
        fprintf(stderr, "Error: Failed to read register '%s' at node %u (addr: 0x%08X)\n", 
                reg_name, node_id, addr);
        return -1;
    }
    
    return 0;
}

RealEmu_Device* realemu_device_init(char *h2c_dev, char *c2h_dev, char *user_dev){
    if (h2c_dev == NULL || c2h_dev == NULL || user_dev == NULL) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return NULL;
    }

    RealEmu_Device* realemu_device = (RealEmu_Device *)malloc(sizeof(RealEmu_Device));
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for RealEmu_Device\n");
        return NULL;
    }

    realemu_device->xdma_h2c_fd = open(h2c_dev, O_RDWR);
    if (realemu_device->xdma_h2c_fd < 0) {
        fprintf(stderr, "Error: Failed to open H2C device %s: %s\n", h2c_dev, strerror(errno));
        free(realemu_device);
        return NULL;
    }

    realemu_device->xdma_c2h_fd = open(c2h_dev, O_RDWR);
    if (realemu_device->xdma_c2h_fd < 0) {
        fprintf(stderr, "Error: Failed to open C2H device %s: %s\n", c2h_dev, strerror(errno));
        close(realemu_device->xdma_h2c_fd);
        free(realemu_device);
        return NULL;
    }

    realemu_device->user_reg_fd = open(user_dev, O_RDWR);
    if (realemu_device->user_reg_fd < 0) {
        fprintf(stderr, "Error: Failed to open user register device %s: %s\n", user_dev, strerror(errno));
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        free(realemu_device);
        return NULL;
    }

    realemu_device->node_num = NODE_NUM;

    for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
        realemu_device->tx_queue[i] = NULL;
    }

    for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
        realemu_device->rx_queue[i] = NULL;
    }

    for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
        if (realemu_init_tx_queue(realemu_device, i) != 0) {
            fprintf(stderr, "Error: Failed to initialize TX queue %d\n", i);
            for (int j = 0; j < i; j++) {
                if (realemu_device->tx_queue[j] != NULL) {
                    realemu_tx_queue_clean(realemu_device->tx_queue[j]);
                    free(realemu_device->tx_queue[j]);
                }
            }
            close(realemu_device->xdma_h2c_fd);
            close(realemu_device->xdma_c2h_fd);
            close(realemu_device->user_reg_fd);
            free(realemu_device);
            return NULL;
        }
    }

    for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
        if (realemu_init_rx_queue(realemu_device, i) != 0) {
            fprintf(stderr, "Error: Failed to initialize RX queue %d\n", i);
            for (int j = 0; j < REALEMU_TX_QUEUES; j++) {
                if (realemu_device->tx_queue[j] != NULL) {
                    realemu_tx_queue_clean(realemu_device->tx_queue[j]);
                    free(realemu_device->tx_queue[j]);
                }
            }
            for (int j = 0; j < i; j++) {
                if (realemu_device->rx_queue[j] != NULL) {
                    realemu_rx_queue_clean(realemu_device->rx_queue[j]);
                    free(realemu_device->rx_queue[j]);
                }
            }
            close(realemu_device->xdma_h2c_fd);
            close(realemu_device->xdma_c2h_fd);
            close(realemu_device->user_reg_fd);
            free(realemu_device);
            return NULL;
        }
    }

    // 初始化设备级别的互斥锁
    if (pthread_mutex_init(&realemu_device->h2c_lock, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize h2c_lock\n");
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    if (pthread_mutex_init(&realemu_device->c2h_lock, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize c2h_lock\n");
        pthread_mutex_destroy(&realemu_device->h2c_lock);
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    // 初始化 RX 轮询线程的同步机制
    if (pthread_mutex_init(&realemu_device->rx_lock, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize rx_lock\n");
        pthread_mutex_destroy(&realemu_device->h2c_lock);
        pthread_mutex_destroy(&realemu_device->c2h_lock);
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    if (pthread_cond_init(&realemu_device->rx_cond, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize rx_cond\n");
        pthread_mutex_destroy(&realemu_device->rx_lock);
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    // 创建 eventfd 用于通知外部模块有数据到达
    realemu_device->rx_eventfd = eventfd(0, EFD_NONBLOCK);
    if (realemu_device->rx_eventfd < 0) {
        fprintf(stderr, "Error: Failed to create eventfd: %s\n", strerror(errno));
        pthread_cond_destroy(&realemu_device->rx_cond);
        pthread_mutex_destroy(&realemu_device->rx_lock);
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    // 启动 RX 轮询线程
    realemu_device->rx_poll_running = 1;
    if (pthread_create(&realemu_device->rx_poll_thread, NULL, rx_polling_thread, realemu_device) != 0) {
        fprintf(stderr, "Error: Failed to create RX polling thread\n");
        pthread_cond_destroy(&realemu_device->rx_cond);
        pthread_mutex_destroy(&realemu_device->rx_lock);
        // 清理已分配的资源
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    // 启动 TX flush 线程（持续把 TX 队列写入 H2C）
    realemu_device->tx_flush_running = 1;
    if (pthread_create(&realemu_device->tx_flush_thread, NULL, tx_flush_thread, realemu_device) != 0) {
        fprintf(stderr, "Error: Failed to create TX flush thread\n");
        realemu_device->tx_flush_running = 0;
        realemu_device->rx_poll_running = 0;
        pthread_join(realemu_device->rx_poll_thread, NULL);
        pthread_cond_destroy(&realemu_device->rx_cond);
        pthread_mutex_destroy(&realemu_device->rx_lock);
        if (realemu_device->rx_eventfd >= 0) {
            close(realemu_device->rx_eventfd);
        }
        for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
            if (realemu_device->tx_queue[i] != NULL) {
                realemu_tx_queue_clean(realemu_device->tx_queue[i]);
                free(realemu_device->tx_queue[i]);
            }
        }
        for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
            if (realemu_device->rx_queue[i] != NULL) {
                realemu_rx_queue_clean(realemu_device->rx_queue[i]);
                free(realemu_device->rx_queue[i]);
            }
        }
        close(realemu_device->xdma_h2c_fd);
        close(realemu_device->xdma_c2h_fd);
        close(realemu_device->user_reg_fd);
        free(realemu_device);
        return NULL;
    }

    printf("RX 轮询线程已启动\n");
    printf("TX flush 线程已启动\n");

    return realemu_device;
}


int realemu_send_pkt_data(RealEmu_Device* realemu_device, MacEvent macevent){
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    int queue_id = 0; // 数据包使用TX队列0
    RealEmu_Tx_Queue *tx_queue = realemu_device->tx_queue[queue_id];
    if (tx_queue == NULL) {
        fprintf(stderr, "Error: TX queue %d not initialized\n", queue_id);
        return -1;
    }

    if (tx_queue->count >= REALEMU_QUEUE_DEPTH) {
        fprintf(stderr, "Error: TX queue %d is full\n", queue_id);
        return -1;
    }

    if (pthread_mutex_lock(&tx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock TX queue\n");
        return -1;
    }

    MacBridge_TOP mac_bridge;
    mac_bridge.macEvent = macevent;
    mac_bridge.bridgeTag.notUsed = 0;
    mac_bridge.bridgeTag.control = 0;

    RealEmu_Queue_Data *queue_data = tx_queue->data[tx_queue->head];
    if (queue_data == NULL) {
        fprintf(stderr, "Error: Queue data buffer not allocated\n");
        pthread_mutex_unlock(&tx_queue->lock);
        return -1;
    }

    direct_reverse_mac_bridge_to_buffer(&mac_bridge, queue_data->buffer);

    tx_queue->head = (tx_queue->head + 1) % REALEMU_QUEUE_DEPTH;
    tx_queue->count++;

    pthread_mutex_unlock(&tx_queue->lock);
    return 0;
}

int realemu_send_topo_data(RealEmu_Device* realemu_device, ChannelCfg channel_cfg){
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    int queue_id = 1; // 拓扑数据使用TX队列1
    RealEmu_Tx_Queue *tx_queue = realemu_device->tx_queue[queue_id];
    if (tx_queue == NULL) {
        fprintf(stderr, "Error: TX queue %d not initialized\n", queue_id);
        return -1;
    }

    if (tx_queue->count >= REALEMU_QUEUE_DEPTH) {
        fprintf(stderr, "Error: TX queue %d is full\n", queue_id);
        return -1;
    }

    if (pthread_mutex_lock(&tx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock TX queue\n");
        return -1;
    }

    // 创建并初始化完整的CfgBridge_Topo结构（包含头部）
    CfgBridge_Topo topo_data;
    topo_data.channelCfg = channel_cfg;
    topo_data.bridgeTag.notUsed = 0;
    topo_data.bridgeTag.control = 1;

    RealEmu_Queue_Data *queue_data = tx_queue->data[tx_queue->head];
    if (queue_data == NULL) {
        fprintf(stderr, "Error: Queue data buffer not allocated\n");
        pthread_mutex_unlock(&tx_queue->lock);
        return -1;
    }

    direct_reverse_cfg_bridge_topo_to_buffer(&topo_data, queue_data->buffer);

    tx_queue->head = (tx_queue->head + 1) % REALEMU_QUEUE_DEPTH;
    tx_queue->count++;

    pthread_mutex_unlock(&tx_queue->lock);
    return 0;
}

int realemu_send_per_data(RealEmu_Device* realemu_device, PerCfg per_cfg){
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    int queue_id = 2; // PER数据使用TX队列2
    RealEmu_Tx_Queue *tx_queue = realemu_device->tx_queue[queue_id];
    if (tx_queue == NULL) {
        fprintf(stderr, "Error: TX queue %d not initialized\n", queue_id);
        return -1;
    }

    if (tx_queue->count >= REALEMU_QUEUE_DEPTH) {
        fprintf(stderr, "Error: TX queue %d is full\n", queue_id);
        return -1;
    }

    if (pthread_mutex_lock(&tx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock TX queue\n");
        return -1;
    }

    // 创建并初始化完整的CfgBridge_Per结构（包含头部）
    CfgBridge_Per per_data;
    per_data.perCfg = per_cfg;
    per_data.bridgeTag.notUsed = 0;
    per_data.bridgeTag.control = 1;

    RealEmu_Queue_Data *queue_data = tx_queue->data[tx_queue->head];
    if (queue_data == NULL) {
        fprintf(stderr, "Error: Queue data buffer not allocated\n");
        pthread_mutex_unlock(&tx_queue->lock);
        return -1;
    }

    direct_reverse_cfg_bridge_per_to_buffer(&per_data, queue_data->buffer);

    tx_queue->head = (tx_queue->head + 1) % REALEMU_QUEUE_DEPTH;
    tx_queue->count++;

    pthread_mutex_unlock(&tx_queue->lock);
    return 0;
}

int realemu_get_rx_eventfd(RealEmu_Device* realemu_device) {
    if (realemu_device == NULL) {
        return -1;
    }
    return realemu_device->rx_eventfd;
}

int realemu_handle_tx_queue(RealEmu_Device* realemu_device){
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    int total_sent = 0;

    // 循环轮询三个TX队列
    for (int qid = 0; qid < REALEMU_TX_QUEUES; qid++) {
        RealEmu_Tx_Queue *tx_queue = realemu_device->tx_queue[qid];
        if (tx_queue == NULL) {
            continue;
        }

        // 加锁检查队列状态
        if (pthread_mutex_lock(&tx_queue->lock) != 0) {
            fprintf(stderr, "Error: Failed to lock TX queue %d\n", qid);
            continue;
        }

        // 检查队列中是否有数据
        while (tx_queue->count > 0) {
            RealEmu_Queue_Data *queue_data = tx_queue->data[tx_queue->tail];
            if (queue_data == NULL) {
                break;
            }

            // 使用设备级别的锁保护 H2C 设备访问
            pthread_mutex_lock(&realemu_device->h2c_lock);
            
            // 使用dma_with_device函数写入H2C设备（持有锁）
            ssize_t bytes_written = write_bits_from_host_to_device(
                realemu_device->xdma_h2c_fd, 
                (char*)queue_data->buffer, 
                DATA_WIDTH_BYTE
            );

            pthread_mutex_unlock(&realemu_device->h2c_lock);

            if (bytes_written != DATA_WIDTH_BYTE) {
                fprintf(stderr, "Error: Failed to write to H2C device, wrote %zd bytes\n", bytes_written);
                tx_queue->xdma_tx_stats.xdma_xmit_err++;
                break;
            }

            // 更新队列指针和计数
            tx_queue->tail = (tx_queue->tail + 1) % REALEMU_QUEUE_DEPTH;
            tx_queue->count--;
            tx_queue->xdma_tx_stats.xdma_xmit++;
            total_sent++;
        }

        pthread_mutex_unlock(&tx_queue->lock);
    }

    return total_sent;
}

static int realemu_update_rx_queue(RealEmu_Device* realemu_device){
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    // 只处理队列0（数据包接收队列）
    int qid = 0;
    RealEmu_Rx_Queue *rx_queue = realemu_device->rx_queue[qid];
    if (rx_queue == NULL) {
        return 0;
    }

    // 先从硬件读取数据，减少锁持有时间
    uint8_t temp_buffer[DATA_WIDTH_BYTE];
    ssize_t bytes_read = 0;

    // 使用设备级别的锁保护 C2H 设备访问
    pthread_mutex_lock(&realemu_device->c2h_lock);
    bytes_read = read_bits_from_device_to_host(
        realemu_device->xdma_c2h_fd,
        (char*)temp_buffer,
        DATA_WIDTH_BYTE
    );
    pthread_mutex_unlock(&realemu_device->c2h_lock);

    if (bytes_read <= 0) {
        // 没有数据可读
        return 0;
    }

    if (bytes_read != DATA_WIDTH_BYTE) {
        fprintf(stderr, "Error: Incomplete read from C2H device, read %zd bytes\n", bytes_read);
        return -1;
    }

    // 读取成功后，再加锁 RX 队列
    if (pthread_mutex_lock(&rx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock RX queue %d\n", qid);
        return -1;
    }

    // 检查队列是否已满
    if (rx_queue->count >= REALEMU_QUEUE_DEPTH) {
        pthread_mutex_unlock(&rx_queue->lock);
        return 0;
    }

    // 将数据存入队列
    RealEmu_Queue_Data *queue_data = rx_queue->data[rx_queue->head];
    if (queue_data == NULL) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1;
    }

    memcpy(queue_data->buffer, temp_buffer, DATA_WIDTH_BYTE);

    // 更新队列指针和计数
    rx_queue->head = (rx_queue->head + 1) % REALEMU_QUEUE_DEPTH;
    rx_queue->count++;
    rx_queue->xdma_rx_stats.xdma_recv++;

    pthread_mutex_unlock(&rx_queue->lock);

    return 1;
}

static int realemu_rx_poll(RealEmu_Device* realemu_device) {
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Invalid device pointer\n");
        return -1;
    }

    // 只处理队列0（数据包接收队列）
    int qid = 0;
    RealEmu_Rx_Queue *rx_queue = realemu_device->rx_queue[qid];
    if (rx_queue == NULL) {
        return 0;
    }

    // 先从硬件读取数据，减少锁持有时间
    uint8_t temp_buffer[DATA_WIDTH_BYTE];
    ssize_t bytes_read = 0;

    // 使用设备级别的锁保护 C2H 设备访问
    pthread_mutex_lock(&realemu_device->c2h_lock);
    bytes_read = read_bits_from_device_to_host(
        realemu_device->xdma_c2h_fd,
        (char*)temp_buffer,
        DATA_WIDTH_BYTE
    );
    pthread_mutex_unlock(&realemu_device->c2h_lock);

    if (bytes_read <= 0) {
        // 没有数据可读
        return 0;
    }

    if (bytes_read != DATA_WIDTH_BYTE) {
        fprintf(stderr, "Error: Incomplete read from C2H device, read %zd bytes\n", bytes_read);
        return -1;
    }

    // 读取成功后，再加锁 RX 队列
    if (pthread_mutex_lock(&rx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock RX queue %d\n", qid);
        return -1;
    }

    // 检查队列是否已满
    if (rx_queue->count >= REALEMU_QUEUE_DEPTH) {
        pthread_mutex_unlock(&rx_queue->lock);
        return 0;
    }

    // 将数据存入队列
    RealEmu_Queue_Data *queue_data = rx_queue->data[rx_queue->head];
    if (queue_data == NULL) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1;
    }

    memcpy(queue_data->buffer, temp_buffer, DATA_WIDTH_BYTE);

    // 更新队列指针和计数
    rx_queue->head = (rx_queue->head + 1) % REALEMU_QUEUE_DEPTH;
    rx_queue->count++;
    rx_queue->xdma_rx_stats.xdma_recv++;

    pthread_mutex_unlock(&rx_queue->lock);

    return 1;
}

int realemu_handle_rx_queue(RealEmu_Device* realemu_device, MacEvent* macevent){
    if (realemu_device == NULL || macevent == NULL) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    // 使用RX队列0（假设数据包接收队列是0）
    int qid = 0;
    RealEmu_Rx_Queue *rx_queue = realemu_device->rx_queue[qid];
    if (rx_queue == NULL) {
        fprintf(stderr, "Error: RX queue %d not initialized\n", qid);
        return -1;
    }

    // 加锁检查队列状态
    if (pthread_mutex_lock(&rx_queue->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock RX queue %d\n", qid);
        return -1;
    }

    // 检查队列中是否有数据
    if (rx_queue->count == 0) {
        pthread_mutex_unlock(&rx_queue->lock);
        return 0; // 没有数据
    }

    // 获取队列数据
    RealEmu_Queue_Data *queue_data = rx_queue->data[rx_queue->tail];
    if (queue_data == NULL) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1;
    }

    // 硬件 RX 回传为纯 MacEvent（无 bridgeTag 头）
    buffer_to_mac_event(queue_data->buffer, macevent);

    // 更新队列指针和计数
    rx_queue->tail = (rx_queue->tail + 1) % REALEMU_QUEUE_DEPTH;
    rx_queue->count--;

    pthread_mutex_unlock(&rx_queue->lock);

    return 1; // 成功处理一个数据包
}

// RX 轮询线程函数（生产者）
static void* rx_polling_thread(void *arg) {
    RealEmu_Device* realemu_device = (RealEmu_Device*)arg;
    printf("RX 轮询线程启动\n");

    while (realemu_device->rx_poll_running) {
        // 轮询 C2H 通道，读取硬件数据到 RX 队列
        int data_count = realemu_rx_poll(realemu_device);

        if (data_count > 0) {
            // 有数据到达，通知外部模块
            uint64_t u = 1;
            write(realemu_device->rx_eventfd, &u, sizeof(u));
        }

        // 1ms 轮询间隔
        usleep(1000);
    }

    printf("RX 轮询线程退出\n");
    return NULL;
}

// TX flush 线程函数（消费者）：把软件 TX 队列持续下发到底层 H2C
static void* tx_flush_thread(void *arg) {
    RealEmu_Device* realemu_device = (RealEmu_Device*)arg;
    printf("TX flush 线程启动\n");

    while (realemu_device->tx_flush_running) {
        int sent_count = realemu_handle_tx_queue(realemu_device);

        if (sent_count <= 0) {
            usleep(1000);
        } else {
            usleep(100);
        }
    }

    printf("TX flush 线程退出\n");
    return NULL;
}

// 从 RX 队列 0 取出数据
int realemu_get_rx_data(RealEmu_Device* realemu_device, MacEvent* macevent){
    if (realemu_device == NULL || macevent == NULL) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    // 从 RX 队列 0 取出数据
    int qid = 0;
    RealEmu_Rx_Queue *rx_queue = realemu_device->rx_queue[qid];
    if (rx_queue == NULL) {
        fprintf(stderr, "Error: RX queue %d not initialized\n", qid);
        return -1;
    }

    pthread_mutex_lock(&rx_queue->lock);

    if (rx_queue->count == 0) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1; // 队列为空
    }

    RealEmu_Queue_Data *queue_data = rx_queue->data[rx_queue->tail];
    if (queue_data == NULL) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1;
    }

    // 硬件 RX 回传为纯 MacEvent（无 bridgeTag 头）
    buffer_to_mac_event(queue_data->buffer, macevent);

    rx_queue->tail = (rx_queue->tail + 1) % REALEMU_QUEUE_DEPTH;
    rx_queue->count--;
    rx_queue->xdma_rx_stats.xdma_recv++;

    pthread_mutex_unlock(&rx_queue->lock);

    return 0;
}

void realemu_device_cleanup(RealEmu_Device* realemu_device) {
    if (realemu_device == NULL) {
        return;
    }
    
    // 停止 TX flush 线程
    printf("停止 TX flush 线程...\n");
    realemu_device->tx_flush_running = 0;
    pthread_join(realemu_device->tx_flush_thread, NULL);
    printf("TX flush 线程已停止\n");

    // 停止 RX 轮询线程
    printf("停止 RX 轮询线程...\n");
    realemu_device->rx_poll_running = 0;
    pthread_join(realemu_device->rx_poll_thread, NULL);
    printf("RX 轮询线程已停止\n");
    
    // 销毁同步机制
    pthread_cond_destroy(&realemu_device->rx_cond);
    pthread_mutex_destroy(&realemu_device->rx_lock);

    // 关闭 eventfd
    if (realemu_device->rx_eventfd >= 0) {
        close(realemu_device->rx_eventfd);
    }

    // 销毁设备级别的互斥锁
    pthread_mutex_destroy(&realemu_device->h2c_lock);
    pthread_mutex_destroy(&realemu_device->c2h_lock);

    // 关闭文件描述符
    if (realemu_device->xdma_h2c_fd >= 0) {
        close(realemu_device->xdma_h2c_fd);
    }
    if (realemu_device->xdma_c2h_fd >= 0) {
        close(realemu_device->xdma_c2h_fd);
    }
    if (realemu_device->user_reg_fd >= 0) {
        close(realemu_device->user_reg_fd);
    }
    
    // 清理TX队列
    for (int i = 0; i < REALEMU_TX_QUEUES; i++) {
        if (realemu_device->tx_queue[i] != NULL) {
            // 清理队列数据
            for (int j = 0; j < REALEMU_QUEUE_DEPTH; j++) {
                if (realemu_device->tx_queue[i]->data[j] != NULL) {
                    free(realemu_device->tx_queue[i]->data[j]);
                }
            }
            pthread_mutex_destroy(&realemu_device->tx_queue[i]->lock);
            free(realemu_device->tx_queue[i]);
        }
    }
    
    // 清理RX队列
    for (int i = 0; i < REALEMU_RX_QUEUES; i++) {
        if (realemu_device->rx_queue[i] != NULL) {
            // 清理队列数据
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

#ifndef BUILD_LIBRARY

int main(){
    RealEmu_Device *realemu_device = NULL;
    MacEvent tx_macevent, rx_macevent;
    int ret;

    // 0. 初始化设备
    printf("=== 初始化 RealEmu 设备 ===\n");
    realemu_device = realemu_device_init("/dev/xdma0_h2c_0", "/dev/xdma0_c2h_0", "/dev/xdma0_user");
    if (realemu_device == NULL) {
        fprintf(stderr, "Error: Failed to initialize RealEmu device\n");
        return 1;
    }
    printf("设备初始化成功，node_num: %d\n\n", realemu_device->node_num);

    // 寄存器读写测试
    printf("=== 寄存器读写测试 ===\n");
    uint32_t retry_limit_value;
    
    // 1. 查询节点0中Retry limit的值
    ret = realemu_read_reg(realemu_device, 0, "RETRY_LIMIT", &retry_limit_value);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to read RETRY_LIMIT register\n");
    } else {
        printf("节点0的RETRY_LIMIT寄存器当前值: %u\n", retry_limit_value);
    }
    
    // 2. 把值改成6
    printf("将RETRY_LIMIT寄存器值设置为6...\n");
    ret = realemu_write_reg(realemu_device, 0, "RETRY_LIMIT", 6);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to write RETRY_LIMIT register\n");
    } else {
        printf("RETRY_LIMIT寄存器写入成功\n");
    }
    
    // 3. 再查一下
    ret = realemu_read_reg(realemu_device, 0, "RETRY_LIMIT", &retry_limit_value);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to read RETRY_LIMIT register after write\n");
    } else {
        printf("节点0的RETRY_LIMIT寄存器新值: %u\n\n", retry_limit_value);
    }

    // 3. 生成一个数据包
    printf("=== 生成测试数据包 ===\n");
    memset(&tx_macevent, 0, sizeof(MacEvent));
    tx_macevent.status = 1;
    tx_macevent.mpduDigest.mpducacheaddr = 0x1234567890ABCDEF;
    tx_macevent.mpduDigest.mpdulen = 1500;
    tx_macevent.mpduDigest.duration = 2000;
    tx_macevent.mpduDigest.framesubtype = 0;
    tx_macevent.mpduDigest.frametype = 2;
    tx_macevent.rfParam.mcs = 7;
    tx_macevent.rfParam.power = 1000;
    tx_macevent.dstMacId = 0;
    tx_macevent.srcMacId = 1;

    printf("生成的数据包:\n");
    printf("  status: %u\n", tx_macevent.status);
    printf("  mpducacheaddr: 0x%lX\n", tx_macevent.mpduDigest.mpducacheaddr);
    printf("  mpdulen: %u\n", tx_macevent.mpduDigest.mpdulen);
    printf("  duration: %u\n", tx_macevent.mpduDigest.duration);
    printf("  framesubtype: %u\n", tx_macevent.mpduDigest.framesubtype);
    printf("  frametype: %u\n", tx_macevent.mpduDigest.frametype);
    printf("  mcs: %u\n", tx_macevent.rfParam.mcs);
    printf("  power: %u\n", tx_macevent.rfParam.power);
    printf("  dstMacId: %u\n", tx_macevent.dstMacId);
    printf("  srcMacId: %u\n\n", tx_macevent.srcMacId);

    // 4. 把数据包放到队列
    printf("=== 将数据包放入TX队列 ===\n");
    ret = realemu_send_pkt_data(realemu_device, tx_macevent);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to send packet data\n");
        goto cleanup;
    }
    printf("数据包成功放入TX队列\n\n");

    // 5. 把队列中的数据发到设备中
    printf("=== 发送数据到设备 ===\n");
    int sent_count = realemu_handle_tx_queue(realemu_device);
    if (sent_count < 0) {
        fprintf(stderr, "Error: Failed to handle TX queue\n");
        goto cleanup;
    }
    printf("成功发送 %d 个数据包到设备\n\n", sent_count);

    // 6. 从设备中更新rx队列
    printf("=== 从设备更新RX队列 ===\n");
    // 等待一小段时间让数据通过设备
    usleep(10000); // 10ms
    int received_count = realemu_update_rx_queue(realemu_device);
    if (received_count < 0) {
        fprintf(stderr, "Error: Failed to update RX queue\n");
        goto cleanup;
    }
    printf("从设备接收到 %d 个数据包\n\n", received_count);

    // 7. 从rx队列中取出数据并打印
    printf("=== 从RX队列取出数据 ===\n");
    ret = realemu_handle_rx_queue(realemu_device, &rx_macevent);
    if (ret == 0) {
        printf("RX队列中没有数据\n\n");
    } else if (ret == 1) {
        printf("成功从RX队列取出数据:\n");
        printf("  status: %u\n", rx_macevent.status);
        printf("  mpducacheaddr: 0x%lX\n", rx_macevent.mpduDigest.mpducacheaddr);
        printf("  mpdulen: %u\n", rx_macevent.mpduDigest.mpdulen);
        printf("  duration: %u\n", rx_macevent.mpduDigest.duration);
        printf("  framesubtype: %u\n", rx_macevent.mpduDigest.framesubtype);
        printf("  frametype: %u\n", rx_macevent.mpduDigest.frametype);
        printf("  mcs: %u\n", rx_macevent.rfParam.mcs);
        printf("  power: %u\n", rx_macevent.rfParam.power);
        printf("  dstMacId: %u\n", rx_macevent.dstMacId);
        printf("  srcMacId: %u\n\n", rx_macevent.srcMacId);
    } else {
        fprintf(stderr, "Error: Failed to handle RX queue\n");
        goto cleanup;
    }

cleanup:
    // 8. 关闭文件销毁分配的内存空间
    printf("=== 清理资源 ===\n");
    realemu_device_cleanup(realemu_device);

    return 0;
}

#endif