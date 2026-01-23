#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include "realemu_types.h"

//伪装硬件存在，全通逻辑，为上层提供等效接口
// 假设其他头文件和结构体定义已包含（如 MacBridge_TOP, CfgBridge_TOP 等）
// 待验证调试

int main() {
    // 1. 创建发送端和接收端的缓冲区
    uint8_t *tx_buffer = create_bit_buffer();
    uint8_t *rx_buffer = create_bit_buffer();
    if (!tx_buffer || !rx_buffer) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return -1;
    }

    // 2. 模拟发送端：填充数据到 tx_buffer
    MacBridge_TOP tx_data = {
        .macEvent = {
            .status = 1,
            .mpduDigest = {
                .mpducacheaddr = 0x123456789ABCDEF0,
                .mpdulen = 1500,
                .duration = 100,
                .framesubtype = 0xA,
                .frametype = 0x3
            },
            .rfParam = {
                .mcs = 0x5,
                .power = 0x1FF
            },
            .dstMacId = 0x2AA,
            .srcMacId = 0x3FF
        },
        .bridgeTag = {
            .notUsed = 0,
            .control = 1
        }
    };
    direct_reverse_mac_bridge_to_buffer(&tx_data, tx_buffer);

    // 3. 模拟接收端：直接从 tx_buffer 读取数据到 rx_buffer
    // （这里简单地将 tx_buffer 复制到 rx_buffer，模拟 DMA 传输）
    memcpy(rx_buffer, tx_buffer, BUFFER_SIZE);

    // 4. 解析接收到的数据
    MacBridge_TOP rx_data;
    buffer_to_mac_bridge(rx_buffer, &rx_data);

    // 5. 验证数据一致性
    printf("Verifying data...\n");
    if (memcmp(&tx_data, &rx_data, sizeof(MacBridge_TOP)) == 0) {
        printf("Test passed: Sent and received data match!\n");
    } else {
        printf("Test failed: Data mismatch!\n");
    }

    // 6. 打印调试信息（可选）
    printf("Sent data:\n");
    print_buffer_in_binary(tx_buffer, BUFFER_SIZE);
    printf("Received data:\n");
    print_buffer_in_binary(rx_buffer, BUFFER_SIZE);

    // 7. 释放资源
    free(tx_buffer);
    free(rx_buffer);
    return 0;
}
