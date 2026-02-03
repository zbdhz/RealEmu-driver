#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "realemu_hw.h"
#include "../tools/reg_rw.h"
#include "../tools/dma_with_device.h"
#include "../tools/dma_utils.h"

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

static int realemu_init_tx_queue(struct realemu_device *realemu_device, u16 qid) {
    if (realemu_device == NULL || qid >= REALEMU_MAX_TX_QUEUES) {
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

static int realemu_init_rx_queue(struct realemu_device *realemu_device, u16 qid) {
    if (realemu_device == NULL || qid >= REALEMU_MAX_RX_QUEUES) {
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
    const RegisterInfo *reg_info = NULL;
    
    if (mac_reg != NULL) {
        if ((mac_reg->access & REG_ACCESS_WO) == 0) {
            fprintf(stderr, "Warning: MAC register '%s' is not writable\n", reg_name);
        }
        addr = realemu_get_mac_reg_addr(node_id, mac_reg->offset);
        reg_info = mac_reg;
    } else if (phy_reg != NULL) {
        if ((phy_reg->access & REG_ACCESS_WO) == 0) {
            fprintf(stderr, "Warning: PHY register '%s' is not writable\n", reg_name);
        }
        addr = realemu_get_phy_reg_addr(node_id, phy_reg->offset);
        reg_info = phy_reg;
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
    const RegisterInfo *reg_info = NULL;
    
    if (mac_reg != NULL) {
        if ((mac_reg->access & REG_ACCESS_RO) == 0) {
            fprintf(stderr, "Warning: MAC register '%s' is not readable\n", reg_name);
        }
        addr = realemu_get_mac_reg_addr(node_id, mac_reg->offset);
        reg_info = mac_reg;
    } else if (phy_reg != NULL) {
        if ((phy_reg->access & REG_ACCESS_RO) == 0) {
            fprintf(stderr, "Warning: PHY register '%s' is not readable\n", reg_name);
        }
        addr = realemu_get_phy_reg_addr(node_id, phy_reg->offset);
        reg_info = phy_reg;
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

int main(){
    int fd = -1;
    if (fd < 0) {
        return -1;
    }
    char rx_buffer[512];
    read_bits_from_device_to_host(fd, rx_buffer, 512);
    return 0;
}
