#include <fcntl.h>
#include <stdint.h>


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



int main(){
    int fd = -1;
    if (fd < 0) {
        return -1;
    }
    char rx_buffer[512];
    read_bits_from_device_to_host(fd, rx_buffer, 512);
    return 0;
}
