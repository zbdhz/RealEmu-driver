#include <stdint.h>
#include <stddef.h>

#include "realemu_top.h"

#define REALEMU_MAX_QUEUE_DEPTH 128
#define REALEMU_MAX_TX_QUEUES 3
#define REALEMU_MAX_RX_QUEUES 1



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

// CfgBridge_Topo 结构体
typedef struct {
    ChannelCfg channelCfg;
    BridgeTag bridgeTag;   // BridgeTag 结构体
} CfgBridge_Topo;

// PerCfg 结构体
typedef struct {
    uint16_t distance:10;     // 10位 (NodeDistance)
    uint16_t dstPhyId:10;     // 10位 (PhyId 来自 MacId)
    uint16_t srcPhyId:10;     // 10位 (PhyId 来自 MacId)
} PerCfg;

// CfgBridge_Per 结构体
typedef struct {
    ChannelCfg channelCfg;
    BridgeTag bridgeTag;   // BridgeTag 结构体
} CfgBridge_TOPO;





void set_bit(uint8_t* buffer, size_t bit_pos, uint8_t value);
void set_bits(uint8_t* buffer, size_t start_bit, size_t num_bits, uint64_t value);

void direct_reverse_mac_bridge_to_buffer(const MacBridge_TOP* data, uint8_t* buffer);
void direct_reverse_cfg_bridge_topo_to_buffer(const CfgBridge_Topo* data, uint8_t* buffer);
void direct_reverse_cfg_bridge_per_to_buffer(const CfgBridge_Per* data, uint8_t* buffer);
void buffer_to_mac_bridge(const uint8_t* buffer, MacBridge_TOP* data);

typedef struct realemu_queue_data {
    uint8_t buffer[DATA_WIDTH_BYTE];
} RealEmu_Queue_Data;

typedef struct realemu_tx_queue {
//写一个512位（并非字节)缓存器buffer，用于存储待发送的数据包
    RealEmu_Queue_Data* data[REALEMU_MAX_QUEUE_DEPTH];
    // 队列管理
    u16 qid;            // 队列ID
    u16 head;           // 生产者指针
    u16 tail;           // 消费者指针
    u16 count;          // 当前队列中的数据数量
    u32 state;          // 队列状态
    
    // 同步机制
    pthread_mutex_t lock;
    // 统计信息
    struct {
        u64 xdma_xmit;      // XDMA 发送统计
        u64 xdma_xmit_err;   // XDMA 发送错误统计
    } xdma_tx_stats;
} RealEmu_Tx_Queue;

typedef struct realemu_rx_queue {
//写一个512位的队列，用于存储接收的数据包
    RealEmu_Queue_Data* data[REALEMU_MAX_QUEUE_DEPTH];
    // 队列管理
    u16 qid;            // 队列ID
    u16 head;           // 生产者指针
    u16 tail;           // 消费者指针
    u16 count;          // 当前队列中的数据数量
    u32 state;          // 队列状态
    
    // 同步机制
    pthread_mutex_t lock;
    // 统计信息
    struct {
        u64 xdma_recv;      // XDMA 接收统计
        u64 xdma_recv_err;   // XDMA 接收错误统计
    } xdma_rx_stats;
} RealEmu_Rx_Queue;

typedef struct realemu_device {
    int xdma_h2c_fd;
    int xdma_c2h_fd;
    int user_reg_fd;
    int node_num;
    RealEmu_Tx_Queue *tx_queue[REALEMU_MAX_TX_QUEUES];
    RealEmu_Rx_Queue *rx_queue[REALEMU_MAX_RX_QUEUES];
} RealEmu_Device;

//初始化tx_queue,分配内存空间，把内存空间分配的指针交给realemu_device->tx_queue
static int realemu_init_tx_queue(struct realemu_device *realemu_device, u16 qid);
//初始化rx_queue,分配内存空间，把内存空间分配的指针交给realemu_device->rx_queue
static int realemu_init_rx_queue(struct realemu_device *realemu_device, u16 qid);
static void realemu_tx_queue_clean(struct realemu_tx_queue *q);//清空tx_queue
static void realemu_rx_queue_clean(struct realemu_rx_queue *q);//清空rx_queue


int realemu_device_init(RealEmu_Device* realemu_device, char *fname, char *fname, char *fname);
int send_pkt_data(RealEmu_Device* realemu_device, MacEvent macevent);
int send_topo_data(RealEmu_Device* realemu_device, CfgBridge_TOPO channelcfg);
int send_per_data(RealEmu_Device* realemu_device, CfgBridge_TOP percfg);
int realemu_device_update_tx_queue(RealEmu_Device* realemu_device);
int realemu_device_update_rx_queue(RealEmu_Device* realemu_device);
int handle_recv_pkt_data(RealEmu_Device* realemu_device, MacEvent* macevent);
int handle_regacces_request(RealEmu_Device* realemu_device, uint32_t reg_addr, uint32_t* reg_val);
