# RealEmu-driver 开发计划

## 开发策略

按照以下顺序进行开发：
1. **阶段1**：完善 realemu-hw 模块（底层硬件接口）
2. **阶段2**：实现 realwmediumd 主逻辑（主程序框架）
3. **阶段3**：打通数据流（硬件 ↔ hwsim）
4. **阶段4**：开发 wserver 和 dynamic 模块（配置管理）

---

## 阶段1：完善 realemu-hw 模块

### 目标
完成硬件接口层的核心功能，为上层提供稳定的数据收发接口。

### 1.1 实现 RX 轮询线程

**文件**：`realemu-hw/realemu_hw.c`

**任务**：
- [ ] 在 `RealEmu_Device` 结构中添加 RX 轮询线程相关字段（已部分完成）
  - `pthread_t rx_poll_thread`
  - `int rx_poll_running`
  - `pthread_mutex_t rx_lock`
  - `pthread_cond_t rx_cond`
  
- [ ] 实现 RX 轮询线程函数 `rx_polling_thread()`
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

- [ ] 在 `realemu_device_init()` 中启动 RX 轮询线程
  - 初始化 `rx_lock` 和 `rx_cond`
  - 设置 `rx_poll_running = 1`
  - 调用 `pthread_create()` 启动线程

- [ ] 在 `realemu_device_cleanup()` 中停止 RX 轮询线程
  - 设置 `rx_poll_running = 0`
  - 调用 `pthread_join()` 等待线程结束
  - 销毁 `rx_lock` 和 `rx_cond`

**验收标准**：
- RX 轮询线程能够正常启动和停止
- 能够轮询 C2H 通道并读取数据
- 能够通过条件变量通知有新数据

---

### 1.2 完善 `realemu_handle_rx_queue()` 函数

**文件**：`realemu-hw/realemu_hw.c`

**当前状态**：函数已存在但需要完善

**任务**：
- [ ] 实现从 RX 队列取出数据的逻辑
- [ ] 将 RX 队列中的 buffer 转换为 MacEvent 结构
- [ ] 处理队列为空的情况
- [ ] 添加错误处理

**实现要点**：
```c
int realemu_handle_rx_queue(RealEmu_Device* realemu_device, MacEvent* macevent) {
    if (realemu_device == NULL || macevent == NULL) {
        return -1;
    }
    
    // 从 RX 队列 0 取出数据
    int qid = 0;
    RealEmu_Rx_Queue *rx_queue = realemu_device->rx_queue[qid];
    
    pthread_mutex_lock(&rx_queue->lock);
    
    if (rx_queue->count == 0) {
        pthread_mutex_unlock(&rx_queue->lock);
        return -1; // 队列为空
    }
    
    RealEmu_Queue_Data *queue_data = rx_queue->data[rx_queue->tail];
    
    // 将 buffer 转换为 MacEvent
    direct_reverse_mac_bridge_from_buffer(queue_data->buffer, macevent);
    
    rx_queue->tail = (rx_queue->tail + 1) % REALEMU_QUEUE_DEPTH;
    rx_queue->count--;
    
    pthread_mutex_unlock(&rx_queue->lock);
    
    return 0;
}
```

**验收标准**：
- 能够正确从 RX 队列取出数据
- 能够正确转换 buffer 为 MacEvent
- 队列为空时返回 -1

---

### 1.3 添加帧结构转换函数

**文件**：新建 `realemu-hw/frame_conversion.c/h`

**任务**：
- [ ] 创建 `frame_conversion.h` 头文件
  ```c
  #ifndef FRAME_CONVERSION_H
  #define FRAME_CONVERSION_H
  
  #include "realemu_hw.h"
  #include "../realwmediumd/realwmediumd.h"
  
  // MacEvent 到 frame 的转换
  struct frame* mac_event_to_frame(const MacEvent* macevent);
  
  // frame 到 MacEvent 的转换
  MacEvent frame_to_mac_event(const struct frame* frame);
  
  #endif
  ```

- [ ] 实现 `mac_event_to_frame()` 函数
  - 从 MacEvent 提取 MAC 地址
  - 从 MacEvent 提取数据
  - 从 MacEvent 提取信号强度
  - 构造 frame 结构体
  - 分配内存并填充字段

- [ ] 实现 `frame_to_mac_event()` 函数
  - 从 frame 提取 MAC 地址
  - 从 frame 提取数据
  - 从 frame 提取信号强度
  - 构造 MacEvent 结构体

**验收标准**：
- 能够正确转换 MacEvent 到 frame
- 能够正确转换 frame 到 MacEvent
- 内存管理正确（避免内存泄漏）

---

### 1.4 测试 realemu-hw 模块

**文件**：新建 `realemu-hw/test_hw.c`

**任务**：
- [ ] 编写简单的测试程序
  - 测试设备初始化
  - 测试 RX 轮询线程启动/停止
  - 测试数据发送/接收
  - 测试帧结构转换

- [ ] 编写 Makefile 添加测试目标

**验收标准**：
- 所有测试通过
- 无内存泄漏
- 线程同步正确

---

## 阶段2：实现 realwmediumd 主逻辑

### 目标
实现主程序框架，集成硬件接口，启动必要的线程。

### 2.1 实现 `init_hardware()` 函数

**文件**：`realwmediumd/realwmediumd.c`

**当前状态**：函数已声明但未实现

**任务**：
- [ ] 实现 `init_hardware()` 函数
  ```c
  int init_hardware(struct realwmediumd *ctx) {
      ctx->realemu_device = realemu_init_for_wmediumd();
      if (ctx->realemu_device == NULL) {
          w_logf(ctx, LOG_ERR, "Failed to initialize RealEmu device\n");
          return -1;
      }
      w_logf(ctx, LOG_INFO, "Hardware initialized successfully\n");
      return 0;
  }
  ```

- [ ] 在 `main()` 函数中调用 `init_hardware()`
  - 在加载配置之前
  - 在初始化 Netlink 之前

**验收标准**：
- 硬件能够成功初始化
- RX 轮询线程正常启动
- 日志输出正确

---

### 2.2 实现 RX 处理线程

**文件**：`realwmediumd/realwmediumd.c`

**任务**：
- [ ] 添加全局变量和线程 ID
  ```c
  static pthread_t rx_process_thread;
  static int rx_process_running = 0;
  ```

- [ ] 实现 `start_rx_processing_thread()` 函数
  ```c
  int start_rx_processing_thread(struct realwmediumd *ctx) {
      rx_process_running = 1;
      if (pthread_create(&rx_process_thread, NULL, rx_processing_thread, ctx) != 0) {
          w_logf(ctx, LOG_ERR, "Failed to create RX processing thread\n");
          return -1;
      }
      w_logf(ctx, LOG_INFO, "RX processing thread started\n");
      return 0;
  }
  ```

- [ ] 实现 `stop_rx_processing_thread()` 函数
  ```c
  void stop_rx_processing_thread(void) {
      rx_process_running = 0;
      pthread_join(rx_process_thread, NULL);
  }
  ```

- [ ] 实现 `rx_processing_thread()` 线程函数
  ```c
  static void* rx_processing_thread(void* arg) {
      struct realwmediumd* ctx = (struct realwmediumd*)arg;
      MacEvent macevent;
      
      while (rx_process_running) {
          // 等待 RX 轮询线程的通知
          pthread_mutex_lock(&ctx->realemu_device->rx_lock);
          pthread_cond_wait(&ctx->realemu_device->rx_cond, 
                           &ctx->realemu_device->rx_lock);
          pthread_mutex_unlock(&ctx->realemu_device->rx_lock);
          
          // 处理接收到的数据
          while (realemu_receive_packet(ctx->realemu_device, &macevent) == 0) {
              struct frame* frame = mac_event_to_frame(&macevent);
              if (frame) {
                  deliver_frame(ctx, frame);
              }
          }
      }
      return NULL;
  }
  ```

- [ ] 在 `main()` 函数中启动 RX 处理线程
  - 在启动 wserver 之后
  - 在进入事件循环之前

- [ ] 添加信号处理函数
  - 处理 SIGINT/SIGTERM
  - 调用 `stop_rx_processing_thread()`
  - 调用 `stop_wserver()`

**验收标准**：
- RX 处理线程能够正常启动和停止
- 能够正确接收硬件数据
- 能够正确转换并发送给 hwsim

---

### 2.3 修改帧发送逻辑

**文件**：`realwmediumd/realwmediumd.c`

**任务**：
- [ ] 修改 `deliver_expired_frames()` 函数
  - 在发送帧之前，将 frame 转换为 MacEvent
  - 调用 `realemu_send_packet()` 发送到硬件
  - 保留原有的 Netlink 发送逻辑（用于调试）

- [ ] 添加配置选项控制发送方式
  - 添加命令行参数 `--use-hardware` 或 `--use-netlink`
  - 根据参数选择发送方式

**验收标准**：
- 能够正确将 frame 转换为 MacEvent
- 能够正确发送到硬件
- 兼容原有的 Netlink 发送方式

---

### 2.4 测试主程序

**任务**：
- [ ] 编译 realwmediumd
- [ ] 运行程序（不带 hwsim，只测试硬件初始化）
- [ ] 检查日志输出
- [ ] 检查线程是否正常启动

**验收标准**：
- 程序能够正常启动
- 硬件初始化成功
- 所有线程正常启动
- 无内存泄漏

---

## 阶段3：打通数据流

### 目标
实现完整的硬件 ↔ hwsim 数据流，验证端到端功能。

### 3.1 准备测试环境

**任务**：
- [ ] 确保 XDMA 驱动已加载
- [ ] 确保 FPGA 硬件已编程并连接
- [ ] 确保 mac80211_hwsim 内核模块已加载
- [ ] 准备测试用的 hwsim 虚拟接口

**验收标准**：
- 所有驱动和硬件正常
- hwsim 虚拟接口可用

---

### 3.2 测试下行数据流（hwsim → 硬件）

**任务**：
- [ ] 启动 realwmediumd
- [ ] 使用 hwsim 发送测试帧
- [ ] 监控硬件 TX 队列
- [ ] 检查帧是否正确发送到硬件

**测试方法**：
```bash
# 启动 realwmediumd
sudo ./realwmediumd/realwmediumd -c config.cfg --use-hardware

# 使用 hwsim 发送测试帧
# （需要编写测试脚本）
```

**验收标准**：
- 帧能够从 hwsim 接收
- 帧能够正确转换为 MacEvent
- 帧能够正确发送到硬件 TX 队列

---

### 3.3 测试上行数据流（硬件 → hwsim）

**任务**：
- [ ] 使用 FPGA 发送测试帧
- [ ] 监控 realwmediumd 日志
- [ ] 检查帧是否正确接收
- [ ] 检查帧是否正确发送到 hwsim

**测试方法**：
- 使用 FPGA 内部逻辑发送测试帧
- 或使用回环测试（硬件发送给自己）

**验收标准**：
- 帧能够从硬件接收
- 帧能够正确转换为 frame
- 帧能够正确发送到 hwsim

---

### 3.4 端到端测试

**任务**：
- [ ] 配置两个 hwsim 虚拟接口
- [ ] 在两个接口之间进行 ping 测试
- [ ] 使用 wireshark 或 tcpdump 抓包
- [ ] 验证数据包的正确性

**验收标准**：
- 两个接口能够互相通信
- 数据包内容正确
- 延迟在可接受范围内

---

### 3.5 性能测试

**任务**：
- [ ] 测试不同数据包大小下的性能
- [ ] 测试不同数据速率下的性能
- [ ] 测试并发连接数
- [ ] 记录 CPU 使用率和内存占用

**验收标准**：
- 性能满足设计要求
- CPU 使用率在合理范围
- 无内存泄漏

---

## 阶段4：开发 wserver 和 dynamic 模块

### 目标
实现动态配置管理功能。

### 4.1 实现硬件同步函数

**文件**：`realwmediumd/realwmediumd_dynamic.c`

**任务**：
- [ ] 实现 `sync_station_to_hardware()` 函数
  ```c
  int sync_station_to_hardware(struct realwmediumd *ctx, struct station *station) {
      // 获取站点的硬件 ID
      uint16_t hw_id = get_hw_id_from_mac(station->addr);
      
      // 发送站点信息到硬件
      // （具体实现根据硬件协议）
      
      return 0;
  }
  ```

- [ ] 实现 `sync_topology_to_hardware()` 函数
  ```c
  int sync_topology_to_hardware(struct realwmediumd *ctx) {
      // 遍历所有站点对
      for (int i = 0; i < ctx->num_stas; i++) {
          for (int j = 0; j < ctx->num_stas; j++) {
              int snr = ctx->snr_matrix[i * ctx->num_stas + j];
              
              // 转换 SNR 为距离或其他硬件参数
              uint16_t distance = snr_to_distance(snr);
              
              // 发送拓扑配置到硬件
              realemu_send_topology(ctx->realemu_device, i, j, distance);
          }
      }
      return 0;
  }
  ```

- [ ] 实现 `sync_per_to_hardware()` 函数
  ```c
  int sync_per_to_hardware(struct realwmediumd *ctx, int src_idx, int dst_idx, double per) {
      // 将 PER 转换为硬件格式
      uint16_t hw_per = per_to_hw_value(per);
      
      // 发送 PER 配置到硬件
      realemu_send_per(ctx->realemu_device, src_idx, dst_idx, hw_per);
      
      return 0;
  }
  ```

**验收标准**：
- 能够正确同步站点信息到硬件
- 能够正确同步拓扑配置到硬件
- 能够正确同步 PER 配置到硬件

---

### 4.2 在 wserver 中添加硬件同步调用

**文件**：`realwmediumd/wserver.c`

**任务**：
- [ ] 在 `handle_snr_update_request()` 中添加同步调用
  ```c
  int handle_snr_update_request(...) {
      // 更新 SNR 矩阵
      ...
      
      // 同步到硬件
      sync_topology_to_hardware(ctx);
      
      return 0;
  }
  ```

- [ ] 在 `handle_position_update_request()` 中添加同步调用
- [ ] 在 `handle_txpower_update_request()` 中添加同步调用
- [ ] 在 `handle_gain_update_request()` 中添加同步调用
- [ ] 在 `handle_errprob_update_request()` 中添加同步调用
- [ ] 在 `handle_add_request()` 中添加同步调用
- [ ] 在 `handle_delete_by_id_request()` 中添加同步调用
- [ ] 在 `handle_delete_by_mac_request()` 中添加同步调用

**验收标准**：
- 所有配置更新都能同步到硬件
- 同步操作不影响性能

---

### 4.3 实现 MAC 地址到硬件 ID 映射

**文件**：`realwmediumd/realwmediumd_dynamic.c`

**任务**：
- [ ] 添加映射表结构
  ```c
  struct mac_to_hw_id {
      uint8_t mac_addr[ETH_ALEN];
      uint16_t hw_id;
  };
  
  struct mac_to_hw_id_map {
      struct mac_to_hw_id *entries;
      int count;
      int capacity;
  };
  ```

- [ ] 实现映射表管理函数
  ```c
  int init_hw_id_map(struct mac_to_hw_id_map *map);
  int add_hw_id_mapping(struct mac_to_hw_id_map *map, const uint8_t *mac, uint16_t hw_id);
  int get_hw_id_from_mac(struct mac_to_hw_id_map *map, const uint8_t *mac, uint16_t *hw_id);
  int remove_hw_id_mapping(struct mac_to_hw_id_map *map, const uint8_t *mac);
  void cleanup_hw_id_map(struct mac_to_hw_id_map *map);
  ```

- [ ] 在 `add_station()` 中添加映射
- [ ] 在 `del_station()` 中删除映射

**验收标准**：
- 映射表能够正确维护
- 查找效率高
- 无内存泄漏

---

### 4.4 测试 wserver 和 dynamic 模块

**任务**：
- [ ] 使用 mininet-wifi 测试配置更新
- [ ] 测试站点添加/删除
- [ ] 测试 SNR 更新
- [ ] 测试位置更新
- [ ] 测试 TX 功率更新
- [ ] 测试增益更新
- [ ] 测试误包率更新

**验收标准**：
- 所有配置更新都能正确处理
- 硬件同步正确
- 无内存泄漏

---

### 4.5 集成测试

**任务**：
- [ ] 完整的系统测试
  - 启动 mininet-wifi
  - 启动 realwmediumd
  - 进行动态配置更新
  - 验证数据流正确性

**验收标准**：
- 系统稳定运行
- 所有功能正常
- 性能满足要求

---

## 总结

### 开发时间估算

| 阶段 | 任务数 | 预计时间 |
|-----|-------|---------|
| 阶段1：realemu-hw | 4 | 3-5 天 |
| 阶段2：realwmediumd 主逻辑 | 4 | 3-5 天 |
| 阶段3：数据流测试 | 5 | 5-7 天 |
| 阶段4：wserver 和 dynamic | 5 | 5-7 天 |
| **总计** | 18 | 16-24 天 |

### 关键里程碑

1. **里程碑1**：realemu-hw 模块完成，RX 轮询线程正常工作
2. **里程碑2**：realwmediumd 主程序启动，RX 处理线程正常工作
3. **里程碑3**：端到端数据流打通，硬件 ↔ hwsim 通信正常
4. **里程碑4**：wserver 和 dynamic 模块完成，动态配置功能正常

### 风险和注意事项

1. **硬件依赖**：需要真实的 FPGA 硬件进行测试
2. **线程同步**：多线程编程容易出现死锁和竞态条件，需要仔细测试
3. **性能**：硬件同步操作可能影响性能，需要优化
4. **兼容性**：需要保持与 wmediumd 的接口兼容性

### 后续工作

- 实现 realemu-hwsim（硬件仿真层）
- 添加更全面的单元测试
- 性能优化
- 文档完善
