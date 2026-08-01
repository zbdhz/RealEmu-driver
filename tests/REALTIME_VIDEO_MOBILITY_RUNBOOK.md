# 两节点渐远实时视频仿真使用手册

本文档说明如何运行 [`realtime_video_mobility.sh`](realtime_video_mobility.sh)，在 RealEmu-driver 上仿真两个逐渐远离的无线节点传输实时视频流。

## 1. 场景说明

脚本基于 [`dataflow_smoke.sh`](dataflow_smoke.sh) 的两节点测试流程，自动完成以下工作：

1. 加载两张 `mac80211_hwsim` 无线网卡。
2. 创建发送端和接收端两个 network namespace。
3. 将两张无线网卡配置为同一个 IBSS 网络。
4. 启动 `realwmediumd` 和动态配置 wserver。
5. 使用 FFmpeg 生成或读取实时视频，并通过 UDP 单播发送。
6. 按节点间距离更新 SNR、带宽、时延和丢包率。
7. 持续采集距离、链路质量、ping 和视频接收指标。
8. 检查视频帧是否经过 RealEmu/XDMA 硬件数据面。

数据路径如下：

```text
FFmpeg 发送端
    │ UDP 单播
    ▼
realemu-video-tx / hwsim0
    │
    ▼
realwmediumd ── XDMA H2C ── FPGA RealEmu ── XDMA C2H
    │
    ▼
realemu-video-rx / hwsim1
    │
    ▼
FFmpeg 接收端
```

## 2. 当前实现需要注意的地方

当前 `realwmediumd` 的单播帧会直接交给硬件判决，软件中的 `snr_matrix` 不会控制单播帧的成功率。因此脚本采用两层联动：

- 通过 `/var/run/realwmediumd.sock` 更新双向 SNR，使 RealEmu 软件控制面保留正确的移动链路状态。
- 通过 `tc netem` 动态调整实际单播数据面的带宽、时延和丢包率，使节点远离能够真实影响视频质量。

视频使用 UDP 单播而不是组播，因而通过 netem 后仍然存活的数据包会继续经过 RealEmu/XDMA/FPGA 路径。

## 3. 环境要求

需要满足以下条件：

1. 使用 Linux，并拥有 root 权限。
2. 已安装以下命令：
   - `ip`
   - `iw`
   - `ping`
   - `tc`
   - `modprobe`
   - `ffmpeg`
   - `python3`
3. FFmpeg 包含默认的 `libx264` 编码器，或者运行时通过 `VIDEO_CODEC` 指定其他编码器。
4. XDMA 驱动已加载，并存在以下设备节点：

```text
/dev/xdma0_h2c_0
/dev/xdma0_c2h_0
/dev/xdma0_user
```

5. RealEmu FPGA 已正确加载对应 bitstream。
6. 仓库中的 `realwmediumd` 可以成功构建。

在 Ubuntu/Debian 上可安装脚本使用的常规工具：

```bash
sudo apt update
sudo apt install ffmpeg iproute2 iw iputils-ping kmod python3
```

上述命令不包含 RealEmu 项目自身的编译依赖和 XDMA 驱动安装；这两部分应按照项目 README 和硬件环境完成。

## 4. 构建

进入仓库并构建 `realwmediumd`：

```bash
cd /home/emu/Mininetwifi/RealEmu-driver
make -C realwmediumd
```

确认生成了可执行文件：

```bash
ls -l realwmediumd/realwmediumd
```

确认 XDMA 设备：

```bash
ls -l /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user
```

确认 FFmpeg 编码器：

```bash
ffmpeg -hide_banner -encoders | grep libx264
```

## 5. 使用默认场景

在仓库根目录运行：

```bash
sudo bash tests/realtime_video_mobility.sh
```

默认场景参数如下：

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `DURATION_SEC` | 60 | 仿真和视频发送时长，单位为秒 |
| `UPDATE_INTERVAL_SEC` | 3 | 链路状态更新周期，单位为秒 |
| `START_DISTANCE_M` | 5 | 两节点初始距离，单位为米 |
| `SPEED_MPS` | 1.5 | 两节点间距增加速度，单位为米/秒 |
| `REFERENCE_SNR_DB` | 35 | 初始距离处的参考 SNR |
| `PATH_LOSS_EXPONENT` | 3.0 | 计算 SNR 衰减时使用的路径损耗指数 |
| `VIDEO_SIZE` | `640x360` | 测试视频分辨率 |
| `VIDEO_FPS` | 25 | 视频帧率 |
| `VIDEO_BITRATE_KBIT` | 1500 | 目标视频码率，单位为 kbit/s |
| `VIDEO_CODEC` | `libx264` | FFmpeg 视频编码器 |

默认情况下，脚本使用 FFmpeg 的 `testsrc2` 生成实时测试画面。节点距离从 5 米逐渐增加到约 95 米，链路参数大致变化如下：

| 时间 | 距离 | SNR | 时延 | 丢包率 | 带宽 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 秒 | 5 米 | 35 dB | 2 ms | 0% | 6000 kbit/s |
| 30 秒 | 50 米 | 4 dB | 91 ms | 8.75% | 3300 kbit/s |
| 60 秒 | 95 米 | -3 dB | 180 ms | 35% | 600 kbit/s |

后半段视频出现卡顿、花屏或丢帧属于预期结果。

## 6. 使用真实视频文件

`VIDEO_SOURCE` 必须使用绝对路径。脚本会使用 `-re` 按真实时间循环读取视频：

```bash
VIDEO_SOURCE=/home/emu/videos/input.mp4 \
sudo -E bash tests/realtime_video_mobility.sh
```

如果还要保存接收端收到的 MPEG-TS 视频流：

```bash
VIDEO_SOURCE=/home/emu/videos/input.mp4 \
RECORD_FILE=/tmp/realemu_received.ts \
sudo -E bash tests/realtime_video_mobility.sh
```

仿真结束后可以使用以下命令播放：

```bash
ffplay /tmp/realemu_received.ts
```

由于远距离阶段会主动制造丢包，录制文件中出现解码错误或画面损坏是正常的链路退化表现。

## 7. 自定义移动和链路参数

环境变量需要通过 `sudo -E` 传入脚本。例如运行 90 秒、以 2 m/s 的相对速度远离，并把视频码率降为 1200 kbit/s：

```bash
DURATION_SEC=90 \
START_DISTANCE_M=10 \
SPEED_MPS=2 \
VIDEO_BITRATE_KBIT=1200 \
sudo -E bash tests/realtime_video_mobility.sh
```

可调整的链路边界参数：

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `MIN_DELAY_MS` | 2 | 初始单向 netem 时延 |
| `MAX_DELAY_MS` | 180 | 最远距离处的单向 netem 时延 |
| `MIN_LOSS_PCT` | 0 | 初始丢包率 |
| `MAX_LOSS_PCT` | 35 | 最远距离处的丢包率 |
| `MIN_RATE_KBIT` | 600 | 最远距离处的链路带宽 |
| `MAX_RATE_KBIT` | 6000 | 初始链路带宽 |

例如创建一个退化更加明显的场景：

```bash
MAX_DELAY_MS=250 \
MAX_LOSS_PCT=50 \
MIN_RATE_KBIT=300 \
VIDEO_BITRATE_KBIT=1800 \
sudo -E bash tests/realtime_video_mobility.sh
```

如果 FFmpeg 没有 `libx264`，可以选择另一个已安装的编码器，例如：

```bash
VIDEO_CODEC=mpeg2video \
sudo -E bash tests/realtime_video_mobility.sh
```

## 8. 输出文件

默认情况下，测试结果写入 `tests` 目录：

| 文件 | 内容 |
| --- | --- |
| `realtime_video_mobility.log` | 场景主日志和每次移动更新摘要 |
| `realwmediumd_video.log` | realwmediumd、Netlink 和硬件数据面日志 |
| `video_sender.log` | FFmpeg 发送端日志 |
| `video_receiver.log` | FFmpeg 接收端日志和解码错误 |
| `video_mobility.csv` | 距离、SNR、时延、丢包、带宽、ping 和 RTT 数据 |

CSV 字段说明：

```text
elapsed_s,distance_m,snr_db,delay_ms,loss_pct,rate_kbit,ping_ok,rtt_ms
```

可以覆盖日志路径，但必须确保 root 用户有写权限：

```bash
SCRIPT_LOG=/tmp/realemu_scenario.log \
MOVEMENT_CSV=/tmp/realemu_mobility.csv \
sudo -E bash tests/realtime_video_mobility.sh
```

## 9. 成功判定

成功结束时会看到：

```text
PASS: moving-node real-time video scenario completed
```

脚本会同时检查：

1. 两个节点初始状态下能够互相 ping 通。
2. FFmpeg 发送端正常完成。
3. FFmpeg 接收端至少处理一个视频帧。
4. `realwmediumd_video.log` 中存在 `0->1` 的硬件 TX 记录。
5. 日志中存在 FPGA 返回的硬件完成记录。

可以手工检查硬件数据面计数：

```bash
grep -c '\[TX\] Frame queued to TX queue: 0->1' tests/realwmediumd_video.log
grep -c '\[COMPLETE\] Frame cookie=.*ACKed by hardware' tests/realwmediumd_video.log
```

查看移动过程：

```bash
column -s, -t tests/video_mobility.csv | less -S
```

## 10. 常见问题

### 缺少 XDMA 设备

错误示例：

```text
ERROR: Missing XDMA device: /dev/xdma0_h2c_0
```

检查驱动和 PCIe 设备：

```bash
lsmod | grep xdma
lspci | grep -i xilinx
ls -l /dev/xdma0_*
```

### 找不到 realwmediumd

错误示例：

```text
ERROR: realwmediumd binary not found
```

重新构建：

```bash
make -C realwmediumd
```

### FFmpeg 编码器不可用

查看可用编码器：

```bash
ffmpeg -hide_banner -encoders | less
```

然后通过 `VIDEO_CODEC` 指定可用编码器。

### 初始 ping 失败

检查主日志和 realwmediumd 日志：

```bash
tail -n 80 tests/realtime_video_mobility.log
tail -n 100 tests/realwmediumd_video.log
```

同时确认 FPGA 已加载、XDMA 设备能够读写，且没有另一个 `realwmediumd` 占用硬件。

### 接收帧数为零

检查发送端与接收端日志：

```bash
tail -n 100 tests/video_sender.log
tail -n 100 tests/video_receiver.log
```

如果自定义了链路参数，先降低 `MAX_LOSS_PCT`、提高 `MIN_RATE_KBIT`，并确保初始带宽高于视频码率。

### 网络命名空间已经存在

脚本不会删除同名的已有 namespace，以避免破坏其他实验。确认这些 namespace 不再使用后再手工删除：

```bash
sudo ip netns del realemu-video-tx
sudo ip netns del realemu-video-rx
```

## 11. 清理行为

脚本正常结束、失败或收到 `SIGINT`/`SIGTERM` 时会自动：

1. 终止 FFmpeg 发送端和接收端。
2. 停止 `realwmediumd`。
3. 终止 namespace 内的辅助进程。
4. 删除本次创建的两个 network namespace。
5. 卸载本次加载的 `mac80211_hwsim`。
6. 删除临时配置和 FFmpeg 进度文件。

持久化日志、CSV 和通过 `RECORD_FILE` 指定的录像不会被删除。

> 注意：测试启动时会重新加载 `mac80211_hwsim`。请勿在同一台机器上同时运行其他依赖该模块的无线仿真实验。
