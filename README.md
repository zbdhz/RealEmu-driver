# RealEmu-driver 设计文档

## 1. 项目概述

### 1.1 描述
RealEmu-driver 是一个硬件-软件协同设计系统，用于替代传统的 `wmediumd` 无线介质模拟器。它提供了一个用户空间模块（`realwmediumd`），通过 XDMA（Xilinx DMA）与自定义硬件加速器（`RealEmu-hw`）交互，将数据平面处理卸载到硬件，同时在软件中保持控制平面处理。

### 1.2 架构总结
系统由四个主要模块组成：
- **realwmediumd**：顶层软件模块，提供与 wmediumd 兼容的接口到 hwsim（内核模块）
- **realwmediumd_dynamic**：动态参数管理，处理站点添加/删除和硬件同步
- **wserver**：配置服务器，通过 Unix 套接字接收来自 mininet-wifi 的动态更新
- **realemu-hw**：硬件接口层，管理 XDMA 通信和硬件数据结构

### 1.3 关键特性
- 硬件-软件协同设计以优化性能
- 与 wmediumd 接口兼容（通过 Netlink 与 mac80211_hwsim 通信）
- 通过 wserver 进行动态配置更新
- 具有适当同步机制的线程安全操作
- 支持路径损耗和误包率（PER）模型

---

## 2. 目录结构

```
RealEmu-driver/
├── realwmediumd/                    # 顶层软件模块
│   ├── realwmediumd.c/h             # 主程序，wmediumd 兼容接口
│   ├── config.c/h                   # 配置解析和路径损耗模型
│   ├── ieee80211.h                  # IEEE 802.11 帧定义
│   ├── list.h                       # 链表宏
│   ├── realwmediumd_dynamic.c/h     # 动态参数管理
│   ├── wserver.c/h                  # 配置服务器
│   ├── wserver_messages.c/h         # 消息协议定义
│   ├── wserver_messages_network.c/h # 网络消息序列化
│   └── Makefile
│
├── realemu-hw/                      # 硬件接口层
│   ├── realemu_hw.c/h               # 硬件初始化和数据结构
│   └── Makefile
│
├── realemu-hwsim/                   # 硬件仿真层（尚未实现）
│
├── include/                         # 公共头文件
│   └── realemu_top.h                # 公共常量和设备路径
│
├── test/                            # 单元测试和集成测试
│
├── filetransfer/                    # 文件传输脚本
│
├── tools/                           # XDMA 驱动交互工具
│   ├── dma_utils.c/h                # DMA 缓冲区操作
│   ├── dma_with_device.c/h          # DMA 设备封装
│   └── reg_rw.c/h                   # 寄存器读写工具
│
├── XDMA/                            # Xilinx XDMA 驱动
│
└── Makefile                         # 顶层构建配置
```

---

## 3. 模块设计

### 3.1 realwmediumd 模块

**文件**：`realwmediumd/realwmediumd.c`、`realwmediumd/realwmediumd.h`

**职责**：
- 提供与 wmediumd 兼容的 Netlink 接口到 mac80211_hwsim 内核模块
- 在软件帧结构和硬件 MacEvent 结构之间进行转换
- 管理 RX 处理线程
- 处理帧排队、投递和超时管理
- 加载配置并初始化硬件

**关键接口**：
```c
struct realwmediumd {
    // wmediumd 兼容字段
    struct nl_sock *sock;              // Netlink 套接字
    struct nl_cb *cb;                  // Netlink 回调
    int family_id;                     // Netlink 家族 ID
    int num_stas;                      // 站点数量
    struct list_head stations;         // 站点链表
    struct station **sta_array;        // 站点数组
    
    // 矩阵数据
    int *snr_matrix;                   // SNR 矩阵
    double *error_prob_matrix;         // 误包率矩阵
    double **station_err_matrix;       // 站点特定误包率矩阵
    
    // 硬件
    RealEmu_Device *realemu_device;    // 硬件设备指针
    
    // 配置
    int noise_threshold;
    int fading_coefficient;
    void *path_loss_param;
    
    // 函数指针
    int (*calc_path_loss)(void *, struct station *, struct station *);
    double (*get_error_prob)(struct realwmediumd *, double, unsigned int, u32, int, struct station *, struct station *);
};

int init_hardware(struct realwmediumd *ctx);
int start_rx_processing_thread(struct realwmediumd *ctx);
void queue_frame(struct realwmediumd *ctx, struct station *station, struct frame *frame);
void deliver_frame(struct realwmediumd *ctx, struct frame *frame);
void deliver_expired_frames(struct realwmediumd *ctx);
int init_netlink(struct realwmediumd *ctx);
```

**与 realemu-hw 的交互**：
- `init_hardware()` 调用 `realemu_device_init()` 初始化硬件
- `queue_frame()` 将 frame 转换为 MacEvent 并调用 `realemu_send_pkt_data()`
- RX 处理线程调用 `realemu_handle_rx_queue()` 从硬件接收数据

---

### 3.2 realwmediumd_dynamic 模块

**文件**：`realwmediumd/realwmediumd_dynamic.c`、`realwmediumd/realwmediumd_dynamic.h`

**职责**：
- 管理动态站点操作（添加/删除）
- 当站点添加/删除时调整 SNR 和误包率矩阵大小
- 将参数更改同步到硬件
- 提供对共享数据结构的线程安全访问

**关键接口**：
```c
int add_station(struct realwmediumd *ctx, const u8 addr[]);
int del_station(struct realwmediumd *ctx, struct station *station);
int del_station_by_id(struct realwmediumd *ctx, const i32 id);
int del_station_by_mac(struct realwmediumd *ctx, const u8 *addr);

// 硬件同步（新增）
int sync_station_to_hardware(struct realwmediumd *ctx, struct station *station);
int sync_topology_to_hardware(struct realwmediumd *ctx);
int sync_per_to_hardware(struct realwmediumd *ctx, int src_idx, int dst_idx, double per);
```

**同步**：
- 使用 `snr_lock`（pthread_rwlock_t）保护共享数据结构
- wserver 线程在更新参数时获取写锁
- 主线程在访问参数时获取读锁

**与 realemu-hw 的交互**：
- 参数更新后调用硬件同步函数
- `sync_topology_to_hardware()` 调用 `realemu_send_topology()`
- `sync_per_to_hardware()` 调用 `realemu_send_per()`

---

### 3.3 wserver 模块

**文件**：`realwmediumd/wserver.c`、`realwmediumd/wserver.h`

**职责**：
- 创建 Unix 域套接字用于 mininet-wifi 连接
- 监听 mininet-wifi 连接
- 接受客户端连接并分发请求
- 处理配置更新请求（SNR、位置、TX 功率、增益、误包率）
- 处理站点添加/删除请求
- 调用 realwmediumd_dynamic 函数进行参数更新

**关键接口**：
```c
int start_wserver(struct realwmediumd *ctx);
void stop_wserver(void);

int handle_snr_update_request(struct request_ctx *ctx, const snr_update_request *request);
int handle_position_update_request(struct request_ctx *ctx, const position_update_request *request);
int handle_txpower_update_request(struct request_ctx *ctx, const txpower_update_request *request);
int handle_gain_update_request(struct request_ctx *ctx, const gain_update_request *request);
int handle_errprob_update_request(struct request_ctx *ctx, const errprob_update_request *request);
int handle_add_request(struct request_ctx *ctx, station_add_request *request);
int handle_delete_by_id_request(struct request_ctx *ctx, station_del_by_id_request *request);
int handle_delete_by_mac_request(struct request_ctx *ctx, station_del_by_mac_request *request);
```

**与 realwmediumd_dynamic 的交互**：
- 调用 `add_station()`、`del_station_by_id()`、`del_station_by_mac()`
- 参数更新后调用硬件同步函数

---

### 3.4 realemu-hw 模块

**文件**：`realemu-hw/realemu_hw.c`、`realemu-hw/realemu_hw.h`

**职责**：
- 初始化 XDMA 设备并分配队列
- 在软件结构和硬件位对齐格式之间进行转换
- 管理 RX 轮询线程（在设备初始化期间启动）
- 提供数据发送/接收接口
- 处理寄存器访问用于调试

**关键接口**：
```c
struct RealEmu_Device {
    int xdma_h2c_fd;                  // 主机到卡通道 FD
    int xdma_c2h_fd;                  // 卡到主机通道 FD
    int user_reg_fd;                  // 用户寄存器 FD
    int node_num;
    RealEmu_Tx_Queue *tx_queue[REALEMU_TX_QUEUES];
    RealEmu_Rx_Queue *rx_queue[REALEMU_RX_QUEUES];
    
    // RX 轮询线程
    pthread_t rx_poll_thread;
    int rx_poll_running;
    
    // 同步
    pthread_mutex_t rx_lock;
    pthread_cond_t rx_cond;
};

RealEmu_Device* realemu_device_init(char *h2c_dev, char *c2h_dev, char *user_dev);
void realemu_device_cleanup(RealEmu_Device* realemu_device);

int realemu_send_pkt_data(RealEmu_Device* realemu_device, MacEvent macevent);
int realemu_send_topo_data(RealEmu_Device* realemu_device, ChannelCfg channel_cfg);
int realemu_send_per_data(RealEmu_Device* realemu_device, PerCfg per_cfg);

int realemu_handle_rx_queue(RealEmu_Device* realemu_device, MacEvent* macevent);
int realemu_update_rx_queue(RealEmu_Device* realemu_device);

int realemu_write_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t value);
int realemu_read_reg(RealEmu_Device* realemu_device, uint32_t node_id, const char *reg_name, uint32_t *value);
```

**数据结构**：
- `MacEvent`：用于数据包传输的 MAC 事件结构
- `ChannelCfg`：用于拓扑的信道配置
- `PerCfg`：误包率配置
- `MacBridge_TOP`：带头的 MAC 桥接结构
- `CfgBridge_Topo`：拓扑配置桥接结构
- `CfgBridge_Per`：PER 配置桥接结构

**RX 轮询线程**：
- 在 `realemu_device_init()` 中启动
- 以 1ms 间隔轮询 C2H 通道
- 将接收到的数据放入 RX 队列
- 通过条件变量通知 RX 处理线程

---

## 4. 线程架构

### 4.1 线程概述

| 线程名称 | 位置 | 启动时间 | 主要职责 |
|---------|------|---------|---------|
| 主线程 | realwmediumd.c | 程序启动 | 初始化、事件循环 |
| wserver 线程 | wserver.c | main() → start_wserver() | 处理 mininet-wifi 配置请求 |
| RX 轮询线程 | realemu-hw.c | realemu_device_init() | 轮询硬件 C2H 通道 |
| RX 处理线程 | realwmediumd.c | main() → start_rx_processing_thread() | 处理从硬件接收的数据 |

### 4.2 主线程

**位置**：`realwmediumd.c` - `main()` 函数

**职责**：
1. 解析命令行参数
2. 调用 `init_hardware()` 初始化硬件（启动 RX 轮询线程）
3. 调用 `load_config()` 加载配置文件
4. 调用 `init_netlink()` 初始化 Netlink 通信
5. 调用 `start_wserver()` 启动 wserver 线程
6. 调用 `start_rx_processing_thread()` 启动 RX 处理线程
7. 设置 libevent 定时器
8. 进入 `event_base_dispatch()` 事件循环

**事件循环**：
- 处理 Netlink 消息（接收来自 hwsim 的帧）
- 处理定时器事件（投递过期帧、移动站点）
- 阻塞直到收到终止信号

### 4.3 wserver 线程

**位置**：`wserver.c`

**启动**：主线程调用 `start_wserver(ctx)`

**职责**：
- 创建 Unix 域套接字
- 监听 mininet-wifi 连接
- 接受客户端连接
- 将配置请求分发到处理程序
- 调用 realwmediumd_dynamic 函数
- 调用硬件同步函数

**同步**：
- 使用 `snr_lock`（读写锁）保护共享数据
- 更新 SNR、位置、功率、增益、误包率时获取写锁

### 4.4 RX 轮询线程

**位置**：`realemu-hw/realemu_hw.c`

**启动**：在 `realemu_device_init()` 中启动

**职责**：
- 持续轮询 C2H 通道
- 将硬件数据读取到 RX 队列
- 当数据到达时通过条件变量通知 RX 处理线程
- 当 `rx_poll_running` 设置为 0 时停止

**同步**：
- 使用 `rx_lock`（互斥锁）和 `rx_cond`（条件变量）
- 当数据可用时通知 RX 处理线程

**实现**：
```c
static void* rx_polling_thread(void* arg) {
    RealEmu_Device* device = (RealEmu_Device*)arg;
    while (device->rx_poll_running) {
        int data_count = realemu_update_rx_queue(device);
        if (data_count > 0) {
            pthread_mutex_lock(&device->rx_lock);
            pthread_cond_signal(&device->rx_cond);
            pthread_mutex_unlock(&device->rx_lock);
        }
        usleep(1000); // 1ms 轮询间隔
    }
    return NULL;
}
```

### 4.5 RX 处理线程

**位置**：`realwmediumd.c`

**启动**：主线程调用 `start_rx_processing_thread(ctx)`

**职责**：
- 等待来自 RX 轮询线程的通知（条件变量）
- 从硬件 RX 队列检索 MacEvent 数据
- 将 MacEvent 转换为 frame 结构
- 调用 `deliver_frame()` 通过 Netlink 发送到 hwsim

**同步**：
- 使用 `rx_lock`（互斥锁）和 `rx_cond`（条件变量）
- 在条件变量上等待数据可用性

**实现**：
```c
static void* rx_processing_thread(void* arg) {
    struct realwmediumd* ctx = (struct realwmediumd*)arg;
    MacEvent macevent;
    while (rx_process_running) {
        pthread_mutex_lock(&ctx->realemu_device->rx_lock);
        pthread_cond_wait(&ctx->realemu_device->rx_cond, 
                         &ctx->realemu_device->rx_lock);
        pthread_mutex_unlock(&ctx->realemu_device->rx_lock);
        
        while (realemu_handle_rx_queue(ctx->realemu_device, &macevent) == 0) {
            struct frame* frame = mac_event_to_frame(&macevent);
            if (frame) {
                deliver_frame(ctx, frame);
            }
        }
    }
    return NULL;
}
```

### 4.6 线程启动序列

```
程序启动
    ↓
主线程 (main)
    ↓
init_hardware()
    ↓
realemu_device_init()
    ↓
RX 轮询线程启动 (realemu-hw)
    ↓
加载配置
    ↓
初始化 Netlink
    ↓
start_wserver()
    ↓
wserver 线程启动
    ↓
start_rx_processing_thread()
    ↓
RX 处理线程启动
    ↓
event_base_dispatch()
    ↓
主线程进入事件循环
```

### 4.7 线程关闭序列

```
收到 SIGINT/SIGTERM
    ↓
主线程：event_base_loopexit()
    ↓
主线程：stop_rx_processing_thread()
    ↓
RX 处理线程：rx_process_running = 0，退出
    ↓
主线程：stop_wserver()
    ↓
wserver 线程：pthread_cancel，退出
    ↓
主线程：realemu_device_cleanup()
    ↓
RX 轮询线程：rx_poll_running = 0，退出
    ↓
主线程：清理资源，退出
```

---

## 5. 数据流

### 5.1 上行数据流（硬件 → hwsim）

```
FPGA 硬件
    ↓ C2H 通道
RX 轮询线程 (realemu-hw)
    ↓ 轮询 C2H 通道
realemu_update_rx_queue()
    ↓ 将数据放入 RX 队列
pthread_cond_signal()
    ↓ 通知
RX 处理线程 (realwmediumd)
    ↓ pthread_cond_wait() 被唤醒
realemu_handle_rx_queue()
    ↓ 检索 MacEvent
mac_event_to_frame()
    ↓ 转换为 frame 结构
deliver_frame()
    ↓ Netlink 发送
hwsim (内核)
```

### 5.2 下行数据流（hwsim → 硬件）

```
hwsim (内核)
    ↓ Netlink HWSIM_CMD_FRAME
主线程 (realwmediumd)
    ↓ process_messages_cb() 回调
转换为 frame
    ↓ queue_frame()
定时器回调
    ↓ deliver_expired_frames()
frame_to_mac_event()
    ↓ 转换为 MacEvent
realemu_send_pkt_data()
    ↓ 放入 TX 队列
FPGA 硬件
```

### 5.3 控制数据流（mininet-wifi → 硬件）

```
mininet-wifi
    ↓ Unix 套接字
wserver 线程
    ↓ 调用
realwmediumd_dynamic 函数
    ↓ 更新数据结构
    ↓ 获取 snr_lock
sync_to_hardware()
    ↓ 调用
realemu-hw 函数
    ↓ realemu_send_topo_data() / realemu_send_per_data()
    ↓ 放入 TX 队列
FPGA 硬件
```

---

## 6. 同步机制

### 6.1 snr_lock（读写锁）

**位置**：`realwmediumd_dynamic.c`

**用途**：保护共享数据结构
- SNR 矩阵
- 误包率矩阵
- 站点链表

**使用**：
```c
// wserver 线程：写操作
pthread_rwlock_wrlock(&snr_lock);
// 更新 SNR/位置/功率/增益/误包率
pthread_rwlock_unlock(&snr_lock);

// 主线程：读操作
pthread_rwlock_rdlock(&snr_lock);
// 读取 SNR 矩阵
pthread_rwlock_unlock(&snr_lock);
```

**涉及的线程**：
- wserver 线程（写锁）
- 主线程（读锁）

### 6.2 rx_lock + rx_cond（互斥锁 + 条件变量）

**位置**：`realemu-hw/realemu_hw.h` - `RealEmu_Device` 结构

**用途**：RX 轮询线程和 RX 处理线程之间的同步

**使用**：
```c
// RX 轮询线程：生产者
pthread_mutex_lock(&device->rx_lock);
pthread_cond_signal(&device->rx_cond);
pthread_mutex_unlock(&device->rx_lock);

// RX 处理线程：消费者
pthread_mutex_lock(&device->rx_lock);
pthread_cond_wait(&device->rx_cond, &device->rx_lock);
pthread_mutex_unlock(&device->rx_lock);
```

**涉及的线程**：
- RX 轮询线程（signal）
- RX 处理线程（wait）

### 6.3 TX 队列锁

**位置**：`realemu-hw/realemu_hw.h` - `RealEmu_Tx_Queue` 结构

**用途**：保护 TX 队列并发访问

**使用**：
```c
// 主线程：发送数据
pthread_mutex_lock(&tx_queue->lock);
// 将数据放入队列
pthread_mutex_unlock(&tx_queue->lock);
```

**涉及的线程**：
- 主线程
- 可能的 XDMA 后台线程

### 6.4 RX 队列锁

**位置**：`realemu-hw/realemu_hw.h` - `RealEmu_Rx_Queue` 结构

**用途**：保护 RX 队列并发访问

**使用**：
```c
// RX 轮询线程：放入数据
pthread_mutex_lock(&rx_queue->lock);
// 将数据放入队列
pthread_mutex_unlock(&rx_queue->lock);

// RX 处理线程：获取数据
pthread_mutex_lock(&rx_queue->lock);
// 从队列获取数据
pthread_mutex_unlock(&rx_queue->lock);
```

**涉及的线程**：
- RX 轮询线程（生产者）
- RX 处理线程（消费者）

---

## 7. 关键设计点

### 7.1 MAC 地址到硬件 ID 映射

必须在 realwmediumd_dynamic 中维护一个映射表以将 MAC 地址转换为硬件 ID：

```c
struct mac_to_hw_id {
    uint8_t mac_addr[ETH_ALEN];
    uint16_t hw_id;
};
```

### 7.2 配置同步时机

- **初始加载**：config.c 加载配置后，调用同步函数发送到硬件
- **动态更新**：wserver 处理请求后，调用同步函数
- **站点添加/删除**：realwmediumd_dynamic 操作后，同步硬件配置

### 7.3 帧结构转换

必须实现转换 frame 和 MacEvent 结构的函数：

```c
// frame_conversion.c/h
struct frame* mac_event_to_frame(const MacEvent* macevent);
MacEvent frame_to_mac_event(const struct frame* frame);
```

### 7.4 事件循环设计

主线程使用 libevent 的事件循环而不是单独的线程：
- `event_base_dispatch()` 是一个处理多个事件源的阻塞调用
- 使用 epoll/kqueue 进行高效的 I/O 多路复用
- 在单个线程中处理 Netlink 套接字事件和定时器事件
- 不需要单独的线程来处理 Netlink 或定时器

---

## 8. 实现优先级

### 8.1 高优先级

1. **realemu-hw 中的 RX 轮询线程**
   - 在 `realemu_device_init()` 中启动
   - 实现轮询逻辑和条件变量通知

2. **realwmediumd 中的 RX 处理线程**
   - 实现 `start_rx_processing_thread()`
   - 实现 MacEvent 到 frame 的转换

3. **realwmediumd 中的 init_hardware**
   - 调用 `realemu_device_init()`
   - 启动 RX 处理线程

4. **realwmediumd_dynamic 中的硬件同步**
   - `sync_station_to_hardware()`
   - `sync_topology_to_hardware()`
   - `sync_per_to_hardware()`

5. **wserver 中的硬件同步调用**
   - 在 handle 函数中添加同步调用

### 8.2 中优先级

6. **MAC 到硬件 ID 映射**
   - 实现映射表维护
   - 实现查找和添加函数

7. **帧结构转换**
   - 实现 `mac_event_to_frame()`
   - 实现 `frame_to_mac_event()`

8. **配置同步时机**
   - 在 config.c 中添加初始配置同步
   - 确保动态更新同步

### 8.3 低优先级

9. **realemu-hwsim**（尚未实现）
10. **测试代码**

---

## 9. 依赖关系

### 9.1 模块依赖图

```
mininet-wifi (Python)
    ↓ Unix 套接字
wserver
    ↓ 调用
realwmediumd_dynamic
    ↓ 调用
realemu-hw
    ↓ 使用
tools (XDMA 驱动)
    ↓
FPGA 硬件

hwsim (内核)
    ↓ Netlink
realwmediumd
    ↓ 使用
config
    ↓ 使用
realemu-hw
```

### 9.2 外部依赖

- **libnl**：用于与 mac80211_hwsim 通信的 Netlink 库
- **libevent**：用于 I/O 多路复用和定时器管理的事件循环库
- **libconfig**：配置文件解析库
- **pthread**：POSIX 线程库
- **XDMA 驱动**：用于 FPGA DMA 访问的 Xilinx XDMA 内核驱动

---

## 10. 构建和运行

### 10.1 构建说明

```bash
# 进入 realwmediumd 目录，执行主程序构建
cd realwmediumd
make

# 说明：realwmediumd/Makefile 会自动调用
# ../realemu-hw/Makefile 和 ../tools/Makefile
# 先生成依赖对象，再链接生成 realwmediumd 可执行文件
```

当前 Makefile 的默认行为如下：

- `tools/Makefile`
    - 默认目标 `make`：仅生成库对象文件（`reg_rw.o`、`dma_with_device.o`、`dma_utils.o`）
    - 可选测试目标：`make dma_test`、`make cfg_test`

- `realemu-hw/Makefile`
    - 默认目标 `make`：仅生成库对象文件（`realemu_hw.o`）
    - 可选测试目标：`make test_rtt`、`make test_throughput`

- `realwmediumd/Makefile`
    - 默认目标 `make`：自动调用上面两个目录的 Makefile 生成依赖对象
    - 随后编译本目录源文件并链接生成 `realwmediumd`

- 顶层 `Makefile`
    - 当前仅构建 `tools` 和 `realemu-hw`
    - 不会直接生成 `realwmediumd` 可执行文件

### 10.2 清理构建产物

```bash
# 清理 realwmediumd 及其依赖目录产物
cd realwmediumd
make clean

# 或单独清理某个模块
cd ../tools && make clean
cd ../realemu-hw && make clean
```

### 10.3 运行时要求

- XDMA 内核驱动已加载
- XDMA 设备文件可访问：`/dev/xdma0_h2c_0`、`/dev/xdma0_c2h_0`、`/dev/xdma0_user`
- mac80211_hwsim 内核模块已加载
- FPGA 硬件已编程并连接

### 10.4 运行

```bash
# 启动 realwmediumd
sudo ./realwmediumd/realwmediumd -c config_file.cfg
```

---

## 11. 设计原理

### 11.1 为什么分离 RX 轮询和处理线程？

- **硬件轮询**：必须持续运行而不阻塞，放在 realemu-hw 模块中
- **数据处理**：可能涉及帧转换和 Netlink 通信，分离以避免阻塞轮询
- **同步**：使用条件变量进行高效通知，避免忙等待

### 11.2 为什么使用 libevent 事件循环？

- **效率**：单个线程使用内核级 I/O 多路复用处理多个事件源
- **简洁性**：不需要单独的 Netlink 和定时器线程
- **性能**：避免上下文切换开销

### 11.3 为什么将所有文件保留在 realwmediumd 目录中？

- **简洁性**：单个目录包含所有软件模块减少复杂性
- **可维护性**：更容易导航和理解代码库
- **兼容性**：保持与原始 wmediumd 类似的结构

---

## 12. 未来增强

- 实现 realemu-hwsim 以在没有实际 FPGA 硬件的情况下进行测试
- 为每个模块添加全面的单元测试
- 支持更多路径损耗模型
- 性能监控和统计信息收集
- 基于流量负载的轮询间隔动态调整
