# Distance Smoke Test Runbook

这个测试用例位于 [tests/distance_smoke.sh](tests/distance_smoke.sh)，用于验证两个点在不同距离下是否还能正常通信，并检查 `realwmediumd` 是否把距离变化正确同步到硬件侧。

## 测试目标

1. 近距离场景下，两端可以正常通信，`ping` 成功。
2. 远距离场景下，链路衰减到不可用，`ping` 失败或无法建立正常通信。
3. `realwmediumd` 日志中可以看到近距离阶段的硬件 ACK 完成，远距离阶段至少有帧进入处理路径，但不会形成成功通信。

## 测试前提

1. 以 root 身份运行。
2. 已加载 XDMA 驱动，并且设备节点存在：`/dev/xdma0_h2c_0`、`/dev/xdma0_c2h_0`、`/dev/xdma0_user`。
3. 已安装 `modprobe`、`ip`、`iw`、`ping`。
4. 当前仓库已经完成构建，且 [realwmediumd/realwmediumd](realwmediumd/realwmediumd) 可执行文件存在。

## 一键运行

在仓库根目录执行：

```bash
make -C realwmediumd
sudo bash tests/distance_smoke.sh
```

脚本成功时会输出 `PASS: distance smoke test completed successfully`。

## 用例说明

脚本会启动两张 `mac80211_hwsim` 虚拟网卡，并把它们放进独立网络命名空间中，避免影响主机网络。随后它会分两阶段运行 `realwmediumd`：

1. **近距离阶段**：两点坐标为 `(0, 0, 0)` 和 `(8, 0, 0)`，应能正常 ping 通。
2. **远距离阶段**：两点坐标为 `(0, 0, 0)` 和 `(1023, 0, 0)`，应无法正常通信。

这两个阶段都使用 `path_loss + log_distance` 模型，由 `realwmediumd` 根据站点坐标计算 distance 并同步到硬件。

## 预期结果

1. 近距离阶段 `ping` 成功。
2. 远距离阶段 `ping` 失败。
3. `tests/distance_near_realwmediumd.log` 中出现 `ACKed by hardware`。
4. `tests/distance_far_realwmediumd.log` 中可以看到 `Frame received` 或其他入站处理日志，但最终不会表现为正常连通。

## 失败时看什么

1. 查看脚本日志。

```bash
tail -n 80 tests/distance_smoke.log
```

2. 查看近距离阶段日志。

```bash
tail -n 80 tests/distance_near_realwmediumd.log
```

3. 查看远距离阶段日志。

```bash
tail -n 80 tests/distance_far_realwmediumd.log
```

4. 检查 `mac80211_hwsim` 是否生成了两张接口。

```bash
ls /sys/class/ieee80211
```

## 判定原则

只要近距离通信成功、远距离通信失败，并且日志可以看出 `realwmediumd` 已启动且处理了数据帧，就可以认为这个用例验证了“距离变化会影响软硬件链路通信结果”。