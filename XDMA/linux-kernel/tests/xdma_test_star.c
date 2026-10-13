// xdma_test_star.c
// 星型拓扑（多外围点 -> 中心点0，外围互不可见）直连 XDMA 验证程序
// 参照 xdma_test_bianchi.c 手动往 /dev/xdma 设备发包，不经过内核网络栈
//
// 拓扑配置（control=1 CfgBridge 包，经 H2C 写入）:
//   中心0 <-> 外围i  距离 8（可达）
//   外围i <-> 外围j  距离 1023（不可达）
//
// RTS 通过 FPGA MAC 的 RTS_THRESH 寄存器（AXI-Lite, /dev/xdma0_user）真正开关:
//   rts=1: RTS_THRESH=1,   mpdulen=1600, 计算带宽取 1500 字节/包
//   rts=0: RTS_THRESH=2347(等效关闭), mpdulen=1100, 计算带宽取 1000 字节/包
//
// mcs=7（OFDM 54Mbps 最大速率）, duration 按标准 OFDM 空口时间公式计算
// （与 realwmediumd pkt_duration 一致: preamble16us + signal4us + 4us*n_sym）
//
// 测量: N(1..15) 个节点持续向中心点发包, 统计第 1-2 秒稳定窗口内
//       中心点成功接收的去重包数, 带宽 = 包数 * 有效字节 * 8 / 1s

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>  // 用于chmod函数和权限常量
#include <grp.h>       // 用于getgrnam函数

// 定义与Bluespec结构体对齐的数据结构
#pragma pack(push, 1)

typedef struct {
    uint8_t notUsed:7;
    uint8_t control:1;
} BridgeTag;

typedef struct {
    uint8_t status:1;
    struct {
        uint64_t mpducacheaddr:64;
        uint16_t mpdulen:16;
        uint16_t duration:16;
        uint16_t framesubtype:4;
        uint16_t frametype:2;
    } mpduDigest;
    struct {
        uint16_t mcs:4;
        uint16_t power:12;
    } rfParam;
    uint16_t dstMacId:10;
    uint16_t srcMacId:10;
} MacEvent;

typedef struct {
    MacEvent macEvent;
    BridgeTag bridgeTag;
} MacBridge_TOP;

typedef struct {
    uint16_t distance:10;
    uint16_t dstPhyId:10;
    uint16_t srcPhyId:10;
} ChannelCfg;

typedef struct {
    ChannelCfg channelCfg;
    BridgeTag bridgeTag;
} CfgBridge_TOP;

#pragma pack(pop)


#define BUFFER_SIZE 64
#define DEVICE_H2C "/dev/xdma0_h2c_0"
#define DEVICE_C2H "/dev/xdma0_c2h_0"
#define DEVICE_USER "/dev/xdma0_user"   // AXI-Lite 寄存器空间

// FPGA 节点寄存器布局 (见 realemu-hw/realemu_hw.h)
#define NODE_REG_BASE   0x00100000u
#define NODE_REG_SIZE   0x00000400u
#define REG_OFF_RTS_THRESH 0x02Cu

#define SEND_DELTA_TIME 100   // 每轮发包间隔 us
#define SEND_NUM 100000       // 最大发送轮数（保证覆盖 1-2 秒窗口）
#define SETTLE_US 200000      // 拓扑配置后的等待时间

#define CENTER_MAC_ID 0
#define MAX_NODES 15          // FPGA 侧 NODE_NUM=16, 中心 0 + 外围 1..15

// 星型拓扑距离
#define DIST_REACHABLE 8      // 中心-外围可达
#define DIST_UNREACHABLE 1023 // 外围-外围不可达

// PHY 速率: mcs=7 即 OFDM 54Mbps（最大速率, 见 test_rtt.c mcs_rate_map）
// rate_100kbps: OFDM 各 mcs 速率, 单位 100kbps (6/9/12/18/24/36/48/54 Mbps)
static const int mcs_rate_100kbps[8] = {60, 90, 120, 180, 240, 360, 480, 540};
#define RATE_KBPS mcs_rate_100kbps[TX_MCS & 7]   // 100kbps 为单位

// RTS on/off 包参数
#define RTS_ON_MPDULEN   1600
#define RTS_ON_BYTES     1500   // 计算带宽的有效字节数
#define RTS_OFF_MPDULEN  1100
#define RTS_OFF_BYTES    1000
// RTS 阈值寄存器值（与内核 mac80211 语义一致: 帧长 > 阈值才发 RTS）
#define RTS_THRESH_ON   1      // 2347: 等效关闭; 1: 所有单播帧都走 RTS/CTS
#define RTS_THRESH_OFF  2347

// 诊断用参数覆盖（环境变量 STAR_MCS/STAR_RTS/STAR_N/STAR_POWER）
static int TX_MCS_OVR = -1;
static int RTS_OVR = -1;
static int N_OVR = -1;
static int POWER_OVR = -1;
#define TX_MCS   (TX_MCS_OVR >= 0 ? TX_MCS_OVR : 7)
// 功率经实测校准: mcs=7(54Mbps)下 power=2000 可稳定解调;
// bianchi.c 原值 898(=10*32+578) 仅够 mcs=0(6Mbps); 过大(3500+)反而收不到
#define TX_POWER (POWER_OVR >= 0 ? (uint16_t)POWER_OVR : (uint16_t)2000)
#define N_MAX    (N_OVR > 0 ? N_OVR : MAX_NODES)
#define RTS_SEL  (RTS_OVR >= 0 ? RTS_OVR : 0)

// OFDM 空口时间(us): preamble(16) + signal(4) + 4us * n_sym
// 与 realwmediumd.c pkt_duration() 公式一致; rate 单位 100kbps
static int ofdm_pkt_duration_us(int len_bytes, int rate_100kbps) {
    int n_sym = ((16 + 8 * len_bytes + 6) * 10 + 4 * rate_100kbps - 1)
                / (4 * rate_100kbps);
    return 16 + 4 + 4 * n_sym;
}

#define SEND_TOTAL_NUM_MIN 1
#define SEND_TOTAL_NUM_MAX MAX_NODES
#define SEND_TOTAL_NUM_STEP 1

typedef struct {
    int send_total_num;
    int rts_on;
    int received_packets;
    double bandwidth_mbps;
} TestResult;


void set_bit(uint8_t* buffer, size_t bit_pos, uint8_t value) {
    if (bit_pos >= BUFFER_SIZE * 8) return;
    size_t byte_pos = bit_pos / 8;
    size_t bit_in_byte = bit_pos % 8;
    if (value) {
        buffer[byte_pos] |= (1 << bit_in_byte);
    } else {
        buffer[byte_pos] &= ~(1 << bit_in_byte);
    }
}

void set_bits(uint8_t* buffer, size_t start_bit, size_t num_bits, uint64_t value) {
    for (size_t i = 0; i < num_bits; i++) {
        set_bit(buffer, start_bit + i, (value >> i) & 0x1);
    }
}

void direct_reverse_mac_bridge_to_buffer(const MacBridge_TOP* data, uint8_t* buffer) {
    size_t bit_pos = 0;
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

void direct_reverse_cfg_bridge_to_buffer(const CfgBridge_TOP* data, uint8_t* buffer) {
    size_t bit_pos = 0;
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

void buffer_to_mac_bridge(const uint8_t* buffer, MacBridge_TOP* data) {
    size_t bit_pos = 0;

    data->macEvent.status = (buffer[bit_pos / 8] >> (bit_pos % 8)) & 0x1;
    bit_pos += 1;

    uint64_t mpducacheaddr = 0;
    for (int i = 0; i < 64; i++) {
        mpducacheaddr |= ((uint64_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.mpducacheaddr = mpducacheaddr;
    bit_pos += 64;

    uint16_t mpdulen = 0;
    for (int i = 0; i < 16; i++) {
        mpdulen |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.mpdulen = mpdulen;
    bit_pos += 16;

    uint16_t duration = 0;
    for (int i = 0; i < 16; i++) {
        duration |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.duration = duration;
    bit_pos += 16;

    uint16_t framesubtype = 0;
    for (int i = 0; i < 4; i++) {
        framesubtype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.framesubtype = framesubtype;
    bit_pos += 4;

    uint16_t frametype = 0;
    for (int i = 0; i < 2; i++) {
        frametype |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.mpduDigest.frametype = frametype;
    bit_pos += 2;

    uint16_t mcs = 0;
    for (int i = 0; i < 4; i++) {
        mcs |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.rfParam.mcs = mcs;
    bit_pos += 4;

    uint16_t power = 0;
    for (int i = 0; i < 12; i++) {
        power |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.rfParam.power = power;
    bit_pos += 12;

    uint16_t dstMacId = 0;
    for (int i = 0; i < 10; i++) {
        dstMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.dstMacId = dstMacId;
    bit_pos += 10;

    uint16_t srcMacId = 0;
    for (int i = 0; i < 10; i++) {
        srcMacId |= ((uint16_t)((buffer[(bit_pos + i) / 8] >> ((bit_pos + i) % 8)) & 0x1)) << i;
    }
    data->macEvent.srcMacId = srcMacId;
    bit_pos += 10;
}

// AXI-Lite 寄存器读写（mmap /dev/xdma0_user, 与 tools/reg_rw.c 实现一致）
static int user_reg_write(int user_fd, uint32_t address, uint32_t value) {
    off_t pgsz = sysconf(_SC_PAGESIZE);
    off_t offset = address & (pgsz - 1);
    off_t target_aligned = address & ~(pgsz - 1);

    void *map = mmap(NULL, offset + 4, PROT_READ | PROT_WRITE,
                     MAP_SHARED, user_fd, target_aligned);
    if (map == (void *)-1) {
        perror("mmap user reg failed");
        return -1;
    }
    *((uint32_t *)((char *)map + offset)) = value;
    munmap(map, offset + 4);
    return 0;
}

static int user_reg_read(int user_fd, uint32_t address, uint32_t *value) {
    off_t pgsz = sysconf(_SC_PAGESIZE);
    off_t offset = address & (pgsz - 1);
    off_t target_aligned = address & ~(pgsz - 1);

    void *map = mmap(NULL, offset + 4, PROT_READ | PROT_WRITE,
                     MAP_SHARED, user_fd, target_aligned);
    if (map == (void *)-1) {
        perror("mmap user reg failed");
        return -1;
    }
    *value = *((uint32_t *)((char *)map + offset));
    munmap(map, offset + 4);
    return 0;
}

// 配置所有节点 MAC 的 RTS 阈值寄存器
// thresh=1 -> 所有单播帧走 RTS/CTS; thresh=2347 -> 等效关闭
static int config_rts_threshold(int user_fd, uint32_t thresh) {
    for (int node = 0; node <= MAX_NODES; node++) {
        uint32_t addr = NODE_REG_BASE + node * NODE_REG_SIZE + REG_OFF_RTS_THRESH;
        if (user_reg_write(user_fd, addr, thresh) < 0) return -1;
    }
    // 回读节点0验证
    uint32_t rb = 0;
    if (user_reg_read(user_fd, NODE_REG_BASE + REG_OFF_RTS_THRESH, &rb) == 0) {
        printf("RTS_THRESH 已写入 %u (节点0回读=%u)\n", thresh, rb);
    }
    return 0;
}

// 写一个拓扑配置包 (src, dst 距离 distance)
int write_topo(int h2c_fd, uint8_t* tx_buf, int src, int dst, int distance) {
    CfgBridge_TOP cfg = {
        .bridgeTag = { .control = 1, .notUsed = 0 },
        .channelCfg = {
            .distance = distance,
            .dstPhyId = dst,
            .srcPhyId = src,
        }
    };
    memset(tx_buf, 0, BUFFER_SIZE);
    direct_reverse_cfg_bridge_to_buffer(&cfg, tx_buf);
    ssize_t written = write(h2c_fd, tx_buf, BUFFER_SIZE);
    if (written < 0) {
        perror("拓扑配置包发送失败");
        return -1;
    }
    return 0;
}

// 配置星型拓扑: 中心0 <-> 外围 i(1..max_peri) 距离 8; 外围互不可见 1023; 未知对 1023
// 上电默认所有节点对距离未知(视为1023不可达), 逐对下发即可
int config_star_topology(int h2c_fd, uint8_t* tx_buf, int max_peri) {
    printf("下发星型拓扑: 中心0<->外围距离%d, 外围互不可见%d\n",
           DIST_REACHABLE, DIST_UNREACHABLE);
    for (int i = 1; i <= max_peri; i++) {
        if (write_topo(h2c_fd, tx_buf, 0, i, DIST_REACHABLE) < 0) return -1;
        if (write_topo(h2c_fd, tx_buf, i, 0, DIST_REACHABLE) < 0) return -1;
        for (int j = i + 1; j <= max_peri; j++) {
            if (write_topo(h2c_fd, tx_buf, i, j, DIST_UNREACHABLE) < 0) return -1;
            if (write_topo(h2c_fd, tx_buf, j, i, DIST_UNREACHABLE) < 0) return -1;
        }
    }
    usleep(SETTLE_US);
    printf("星型拓扑配置完成\n");
    return 0;
}

/**
 * @brief 单个测试点: send_total_num 个外围节点 -> 中心节点0
 * @return 时间窗口(第1-2秒)内中心点成功接收的去重包数, -1 表示失败
 */
int run_single_test(int send_total_num, int rts_on) {
    int h2c_fd = open(DEVICE_H2C, O_RDWR);
    int c2h_fd = open(DEVICE_C2H, O_RDWR);
    int user_fd = open(DEVICE_USER, O_RDWR | O_SYNC);

    if (h2c_fd < 0 || c2h_fd < 0 || user_fd < 0) {
        perror("Failed to open XDMA device");
        return -1;
    }

    size_t buf_size = BUFFER_SIZE;
    uint8_t *rx_buf = (uint8_t*)aligned_alloc(4096, buf_size);
    uint8_t *tx_buf = (uint8_t*)aligned_alloc(4096, buf_size);

    if (!rx_buf || !tx_buf) {
        perror("Memory allocation failed");
        close(h2c_fd);
        close(c2h_fd);
        close(user_fd);
        return -1;
    }
    memset(tx_buf, 0, buf_size);

    // 配置星型拓扑（参考脚本未下发拓扑, 上电默认不可达, 必须先配置）
    if (config_star_topology(h2c_fd, tx_buf, MAX_NODES) < 0) {
        close(h2c_fd);
        close(c2h_fd);
        close(user_fd);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    // 真正配置 FPGA MAC 的 RTS 阈值寄存器（先于数据面跑）
    if (config_rts_threshold(user_fd, rts_on ? RTS_THRESH_ON : RTS_THRESH_OFF) < 0) {
        close(h2c_fd);
        close(c2h_fd);
        close(user_fd);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    // 数据包模板: mcs=7 (54Mbps), duration 按标准 OFDM 公式随包长计算
    uint16_t mpdulen = rts_on ? RTS_ON_MPDULEN : RTS_OFF_MPDULEN;
    uint16_t duration = (uint16_t)ofdm_pkt_duration_us(mpdulen, RATE_KBPS);

    MacBridge_TOP bridge_data = {
        .bridgeTag = { .control = 0, .notUsed = 0 },
        .macEvent = {
            .srcMacId = 1,
            .dstMacId = CENTER_MAC_ID,
            .rfParam = { .power = TX_POWER, .mcs = TX_MCS },
            .mpduDigest = {
                .frametype = 2,
                .framesubtype = 0,
                .duration = duration,
                .mpdulen = mpdulen,
                .mpducacheaddr = 0,
            },
            .status = 0,
        }
    };
    printf("数据包: mcs=%d(54Mbps), mpdulen=%u, duration=%uus (RTS_THRESH=%u)\n",
           TX_MCS, mpdulen, duration, rts_on ? RTS_THRESH_ON : RTS_THRESH_OFF);

    // fork 前非阻塞排空 C2H 中上一个测试残留的陈旧数据:
    // 陈旧包带有上一轮的大序列号, 若不排空会污染子进程的去重计数
    // (这正是此前全量首点 N=1 收到 0 包的原因)
    {
        int c2h_flags = fcntl(c2h_fd, F_GETFL, 0);
        fcntl(c2h_fd, F_SETFL, c2h_flags | O_NONBLOCK);
        ssize_t r;
        do { r = read(c2h_fd, rx_buf, buf_size); } while (r > 0);
        fcntl(c2h_fd, F_SETFL, c2h_flags);  // 恢复阻塞模式
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        close(h2c_fd);
        close(c2h_fd);
        close(user_fd);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    pid_t pid = fork();

    if (pid == 0) {
        // 子进程: 阻塞读 C2H, 统计第1-2秒窗口内到达中心点(去重)的包数
        close(pipefd[0]);
        close(h2c_fd);
        free(tx_buf);

        int time_window_count = 0;
        MacBridge_TOP *event = (MacBridge_TOP*)malloc(sizeof(MacBridge_TOP));

        int seq_num[64];
        for (int i = 0; i < 64; i++) seq_num[i] = -1;

        struct timeval start_time, current_time;
        int time_window_started = 0;
        int time_window_ended = 0;

        gettimeofday(&start_time, NULL);

        while (1) {
            gettimeofday(&current_time, NULL);
            double elapsed_total = (current_time.tv_sec - start_time.tv_sec)
                + (current_time.tv_usec - start_time.tv_usec) / 1000000.0;

            if (!time_window_started && elapsed_total >= 1.0) {
                time_window_started = 1;
                // 窗口开始时重置去重表: 隔离启动第一秒内残留的上一轮大序列号
                for (int i = 0; i < 64; i++) seq_num[i] = -1;
            }
            if (time_window_started && !time_window_ended && elapsed_total >= 2.0) {
                time_window_ended = 1;
            }

            ssize_t read_bytes = read(c2h_fd, rx_buf, buf_size);
            if (read_bytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                printf("读取失败, 退出接收进程\n");
                break;
            }

            buffer_to_mac_bridge(rx_buf, event);
            int srcMacId = event->macEvent.srcMacId;
            // 中心点视角: 只统计发送给自己的包(dst=中心), 按 src 去重取最新序列号
            if (event->macEvent.dstMacId == CENTER_MAC_ID
                    && srcMacId >= 1 && srcMacId <= send_total_num) {
                if ((int)event->macEvent.mpduDigest.mpducacheaddr > seq_num[srcMacId]) {
                    seq_num[srcMacId] = event->macEvent.mpduDigest.mpducacheaddr;
                    if (time_window_started && !time_window_ended) {
                        time_window_count++;
                    }
                }
            }

            // 窗口结束后再吸收 0.5 秒在途包, 然后退出
            if (time_window_ended && elapsed_total >= 2.5) {
                break;
            }
        }

        free(event);

        int write_result = write(pipefd[1], &time_window_count, sizeof(time_window_count));
        if (write_result != sizeof(time_window_count)) {
            perror("pipe write failed");
        }
        close(pipefd[1]);
        close(c2h_fd);
        free(rx_buf);
        exit(0);

    } else if (pid > 0) {
        // 父进程: 持续向 send_total_num 个外围节点注入包
        close(pipefd[1]);

        int child_ended = 0;
        uint64_t seq = 0;

        for (int i = 0; i < SEND_NUM; i++) {
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                child_ended = 1;
                break;
            } else if (result == -1) {
                perror("waitpid failed");
                child_ended = 1;
                break;
            }

            seq++;
            for (int j = 1; j <= send_total_num; j++) {
                bridge_data.macEvent.mpduDigest.mpducacheaddr = seq;
                bridge_data.macEvent.srcMacId = j;
                memset(tx_buf, 0, BUFFER_SIZE);
                direct_reverse_mac_bridge_to_buffer(&bridge_data, tx_buf);
                if (write(h2c_fd, tx_buf, BUFFER_SIZE) < 0) {
                    perror("数据包发送失败");
                }
            }
            usleep(SEND_DELTA_TIME);
        }

        // 等待接收进程退出并取回窗口包数
        int received_total = -1;
        int status;
        wait(&status);
        if (WIFEXITED(status)) {
            ssize_t read_result = read(pipefd[0], &received_total, sizeof(received_total));
            if (read_result != sizeof(received_total)) {
                received_total = -1;
            }
        }

        close(pipefd[0]);
        close(h2c_fd);
        close(c2h_fd);
        close(user_fd);
        free(tx_buf);
        free(rx_buf);

        (void)child_ended;
        return received_total;
    }

    return -1;
}

void save_results_to_csv(TestResult* results, int num_results, const char* filename) {
    FILE* csv_file = fopen(filename, "w");
    if (!csv_file) {
        perror("无法打开CSV文件");
        return;
    }

    fprintf(csv_file, "SEND_NODES,RTS,RECEIVED_PACKETS,BANDWIDTH_MBPS\n");
    for (int i = 0; i < num_results; i++) {
        fprintf(csv_file, "%d,%d,%d,%.2f\n",
                results[i].send_total_num,
                results[i].rts_on,
                results[i].received_packets,
                results[i].bandwidth_mbps);
    }

    fclose(csv_file);
    chmod(filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    struct group *grp = getgrnam("gtx");
    if (grp) chown(filename, -1, grp->gr_gid);
    printf("测试结果已保存到: %s\n", filename);
}

int main() {
    // 诊断用环境变量覆盖: STAR_MCS / STAR_RTS / STAR_N / STAR_POWER
    if (getenv("STAR_MCS"))   TX_MCS_OVR = atoi(getenv("STAR_MCS"));
    if (getenv("STAR_RTS"))   RTS_OVR = atoi(getenv("STAR_RTS"));
    if (getenv("STAR_N"))     N_OVR = atoi(getenv("STAR_N"));
    if (getenv("STAR_POWER")) POWER_OVR = atoi(getenv("STAR_POWER"));

    printf("=== 星型拓扑 RTS 直连验证测试开始 ===\n");
    printf("拓扑: %d 个外围节点 -> 中心节点%d, 外围互不可见(距离%d)\n",
           MAX_NODES, CENTER_MAC_ID, DIST_UNREACHABLE);
    printf("PHY: mcs=%d (OFDM 54Mbps 最大速率)\n", TX_MCS);
    printf("RTS on : RTS_THRESH=%d, mpdulen=%d(时长%dus), 带宽按 %d 字节/包计算\n",
           RTS_THRESH_ON, RTS_ON_MPDULEN,
           ofdm_pkt_duration_us(RTS_ON_MPDULEN, RATE_KBPS), RTS_ON_BYTES);
    printf("RTS off: RTS_THRESH=%d, mpdulen=%d(时长%dus), 带宽按 %d 字节/包计算\n",
           RTS_THRESH_OFF, RTS_OFF_MPDULEN,
           ofdm_pkt_duration_us(RTS_OFF_MPDULEN, RATE_KBPS), RTS_OFF_BYTES);
    printf("duration 公式: 16+4+4*n_sym (标准 OFDM, 与 realwmediumd pkt_duration 一致)\n");
    printf("统计窗口: 第1-2秒 (排除启动瞬态)\n\n");

    int rts_from = RTS_SEL, rts_to = RTS_SEL;
    if (RTS_OVR < 0) { rts_from = 0; rts_to = 1; } // 默认两种都跑

    int total_tests = (N_MAX - SEND_TOTAL_NUM_MIN) / SEND_TOTAL_NUM_STEP + 1;
    int total_runs = total_tests * (rts_to - rts_from + 1);

    TestResult* results = (TestResult*)malloc(total_runs * sizeof(TestResult));
    if (!results) {
        perror("内存分配失败");
        return -1;
    }

    int result_index = 0;

    for (int rts_on = rts_from; rts_on <= rts_to; rts_on++) {
        printf("\n########## %s ##########\n", rts_on ? "RTS ON (THRESH=1, mpdulen=1600)" : "RTS OFF (THRESH=2347, mpdulen=1100)");
        for (int send_total_num = SEND_TOTAL_NUM_MIN;
                send_total_num <= N_MAX;
                send_total_num += SEND_TOTAL_NUM_STEP) {
            printf("\n=== 开始测试: N=%d, RTS=%s ===\n", send_total_num, rts_on ? "ON" : "OFF");

            int received_packets = run_single_test(send_total_num, rts_on);

            int bytes = rts_on ? RTS_ON_BYTES : RTS_OFF_BYTES;
            double bw = (received_packets > 0)
                ? received_packets * (double)bytes * 8.0 / 1.0 /* 1 秒窗口 */
                : 0.0;

            results[result_index].send_total_num = send_total_num;
            results[result_index].rts_on = rts_on;
            results[result_index].received_packets = received_packets;
            results[result_index].bandwidth_mbps = bw / 1e6;
            result_index++;

            printf("测试完成: N=%d, RTS=%s, 窗口内包数=%d, 带宽=%.2f Mbps\n",
                   send_total_num, rts_on ? "ON" : "OFF", received_packets,
                   results[result_index - 1].bandwidth_mbps);
        }
    }

    save_results_to_csv(results, total_runs, "star_rts_test_results.csv");

    printf("\n=== 测试汇总: 稳定带宽 (Mbps) ===\n");
    printf("%4s | %12s | %12s\n", "N", "RTS_OFF", "RTS_ON");
    for (int i = 0; i < total_tests; i++) {
        printf("%4d | %12.2f | %12.2f\n",
               results[i].send_total_num,
               results[i].bandwidth_mbps,
               results[total_tests + i].bandwidth_mbps);
    }

    free(results);
    printf("\n=== 星型拓扑 RTS 直连验证测试完成 ===\n");
    return 0;
}
