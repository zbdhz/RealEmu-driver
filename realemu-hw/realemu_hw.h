#include <stdint.h>
#include <stddef.h>

#include "realemu_top.h"


#define REALEMU_QUEUE_DEPTH 1024
#define REALEMU_TX_QUEUES 3
#define REALEMU_RX_QUEUES 1


//寄存器相关配置

#define REALEMU_TOTAL_ADDR_MIN    0x00000000
#define REALEMU_TOTAL_ADDR_MAX    0x001FFFFF
#define REALEMU_ADAPTER_ADDR_MIN  0x00000000
#define REALEMU_ADAPTER_ADDR_MAX  0x000FFFFF
#define REALEMU_NODE_ADDR_MIN     0x00100000
#define REALEMU_NODE_ADDR_MAX     0x001FFFFF

#define REALEMU_NODE_BASE_ADDR    0x00100000
#define REALEMU_NODE_SIZE        0x00000400
#define REALEMU_NODE_COUNT       1024

#define REALEMU_MAC_OFFSET       0x00000000
#define REALEMU_PHY_OFFSET       0x00000200
#define REALEMU_MAC_SIZE        0x00000200
#define REALEMU_PHY_SIZE        0x00000200

typedef enum {
    REG_ACCESS_RO = 0x01,
    REG_ACCESS_WO = 0x02,
    REG_ACCESS_RW = 0x03
} RegAccessType;

typedef enum {
    REG_TYPE_MAC_CONFIG,
    REG_TYPE_MAC_STATUS,
    REG_TYPE_PHY_STATUS
} RegType;

typedef struct {
    const char *name;
    uint32_t offset;
    uint8_t access;
    RegType type;
    const char *description;
} RegisterInfo;

static const RegisterInfo mac_reg_table[] = {
    {"SLOT_TIME",       0x000, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Slot time"},
    {"SIFS",            0x004, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "SIFS"},
    {"DIFS",            0x008, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "DIFS"},
    {"EIFS",            0x00C, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "EIFS"},
    {"SIG_TIME",        0x010, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Signal time"},
    {"OFDM_SYMBOL",     0x014, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "OFDM symbol time"},
    {"MAX_NUM",         0x018, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Max num"},
    {"PHY_DELAY",       0x01C, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "PHY delay"},
    {"TIMEOUT",         0x020, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Timeout"},
    {"CW_MIN",          0x024, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "CW min"},
    {"CW_MAX",          0x028, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "CW max"},
    {"RTS_THRESH",      0x02C, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "RTS threshold"},
    {"RETRY_LIMIT",     0x030, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Retry limit"},
    {"NAV_EN",          0x034, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "NAV enable"},
    {"TXOP_EN",         0x038, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "TXOP enable"},
    {"FILTER_EN",       0x03C, REG_ACCESS_RW, REG_TYPE_MAC_CONFIG, "Filter enable"},
    {"BACKOFF_STATE",   0x040, REG_ACCESS_RO, REG_TYPE_MAC_STATUS,  "Backoff state"},
    {"DCF_STATE",       0x044, REG_ACCESS_RO, REG_TYPE_MAC_STATUS,  "DCF state"},
    {"FIFOIN_DEPTH",    0x048, REG_ACCESS_RO, REG_TYPE_MAC_STATUS,  "FIFO in depth"},
    {"FIFOIN_COUNT",    0x04C, REG_ACCESS_RO, REG_TYPE_MAC_STATUS,  "FIFO in count"},
};

static const RegisterInfo phy_reg_table[] = {
    {"FSM_STATE",       0x200, REG_ACCESS_RO, REG_TYPE_PHY_STATUS, "FSM state"},
    {"CCA_BUSY",        0x204, REG_ACCESS_RO, REG_TYPE_PHY_STATUS, "CCA busy"},
    {"RX_POWER_DBM",    0x208, REG_ACCESS_RO, REG_TYPE_PHY_STATUS, "RX power (dBm)"},
    {"FCS_EN",          0x20C, REG_ACCESS_RW, REG_TYPE_PHY_STATUS, "FCS enable"},
    {"FCS_CORRECT",     0x210, REG_ACCESS_RO, REG_TYPE_PHY_STATUS, "FCS correct"},
};

typedef struct {
    uint32_t node_id;
    const char *node_name;
} NodeInfo;

//寄存器相关配置


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
    uint16_t perIn:14;     
    uint16_t perOut:16;     
} PerCfg;

// CfgBridge_Per 结构体
typedef struct {
    PerCfg perCfg;
    BridgeTag bridgeTag;   // BridgeTag 结构体
} CfgBridge_Per;





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
    RealEmu_Queue_Data* data[REALEMU_QUEUE_DEPTH];
    // 队列管理
    uint16_t qid;            // 队列ID
    uint16_t head;           // 生产者指针
    uint16_t tail;           // 消费者指针
    uint16_t count;          // 当前队列中的数据数量
    uint32_t state;          // 队列状态
    
    // 同步机制
    pthread_mutex_t lock;
    // 统计信息
    struct {
        uint64_t xdma_xmit;      // XDMA 发送统计
        uint64_t xdma_xmit_err;   // XDMA 发送错误统计
    } xdma_tx_stats;
} RealEmu_Tx_Queue;

typedef struct realemu_rx_queue {
//写一个512位的队列，用于存储接收的数据包
    RealEmu_Queue_Data* data[REALEMU_QUEUE_DEPTH];
    // 队列管理
    uint16_t qid;            // 队列ID
    uint16_t head;           // 生产者指针
    uint16_t tail;           // 消费者指针
    uint16_t count;          // 当前队列中的数据数量
    uint32_t state;          // 队列状态
    
    // 同步机制
    pthread_mutex_t lock;
    // 统计信息
    struct {
        uint64_t xdma_recv;      // XDMA 接收统计
        uint64_t xdma_recv_err;   // XDMA 接收错误统计
    } xdma_rx_stats;
} RealEmu_Rx_Queue;

typedef struct realemu_device {
    int xdma_h2c_fd;
    int xdma_c2h_fd;
    int user_reg_fd;
    int node_num;
    RealEmu_Tx_Queue *tx_queue[REALEMU_TX_QUEUES];
    RealEmu_Rx_Queue *rx_queue[REALEMU_RX_QUEUES];
} RealEmu_Device;

// //初始化tx_queue,分配内存空间，把内存空间分配的指针交给realemu_device->tx_queue
// static int realemu_init_tx_queue(RealEmu_Device* realemu_device, uint16_t qid);
// //初始化rx_queue,分配内存空间，把内存空间分配的指针交给realemu_device->rx_queue
// static int realemu_init_rx_queue(RealEmu_Device* realemu_device, uint16_t qid);

// static void realemu_tx_queue_clean(RealEmu_Tx_Queue *q);//清空tx_queue
// static void realemu_rx_queue_clean(RealEmu_Rx_Queue *q);//清空rx_queue


RealEmu_Device* realemu_device_init(char *h2c_dev, char *c2h_dev, char *user_dev);

int send_pkt_data(RealEmu_Device* realemu_device, MacEvent macevent);
int send_topo_data(RealEmu_Device* realemu_device, ChannelCfg channel_cfg);
int send_per_data(RealEmu_Device* realemu_device, PerCfg per_cfg);
int handle_regacces_request(RealEmu_Device* realemu_device, uint32_t reg_addr, uint32_t* reg_val);

int realemu_handle_tx_queue(RealEmu_Device* realemu_device);
int realemu_update_rx_queue(RealEmu_Device* realemu_device);

int realemu_handle_rx_queue(RealEmu_Device* realemu_device, MacEvent* macevent);
int realemu_write_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t value);
int realemu_read_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t *value);
