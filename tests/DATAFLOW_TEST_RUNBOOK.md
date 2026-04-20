# 数据流烟雾测试运行流程

这个测试脚本位于 [tests/dataflow_smoke.sh](tests/dataflow_smoke.sh)，目标是验证 realwmediumd 是否能够把两张 hwsim 接口之间的数据流跑通，并且由 ping 作为最终判定。

## 测试前提

1. 以 root 身份运行。
2. 已加载 XDMA 驱动，并且设备节点存在：`/dev/xdma0_h2c_0`、`/dev/xdma0_c2h_0`、`/dev/xdma0_user`。
3. 已安装 `modprobe`、`ip`、`iw`、`ping`。
4. 当前仓库已经完成构建，且 [realwmediumd/realwmediumd](realwmediumd/realwmediumd) 可执行文件存在。

## 一键运行

在仓库根目录执行：

```bash
make -C realwmediumd
sudo bash tests/dataflow_smoke.sh
```

脚本成功时会输出 `PASS: data flow smoke test completed successfully`。

## 手工运行流程

如果你想逐步执行，可以按下面的命令跑：

1. 构建主程序。

```bash
cd /home/emu/dev/RealEmu-driver
make -C realwmediumd
```

2. 确认设备节点存在。

```bash
ls -l /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user
```

3. 加载两个 hwsim radio。

```bash
sudo modprobe -r mac80211_hwsim
sudo modprobe mac80211_hwsim radios=2
```

4. 配置两个无线接口的 MAC、mesh、IP 和路由。

```bash
sudo bash tests/dataflow_smoke.sh
```

这个脚本内部会自动完成以下动作：

```bash
ip link set <dev> down
ip link set address <mac> dev <dev>
iw dev <dev> set type mesh
iw dev <dev> set channel 36
ip link set <dev> up
iw dev <dev> mesh join realemu-smoke
ip addr flush dev <dev>
ip addr add <ip>/24 dev <dev>
```

5. 启动 realwmediumd。

```bash
sudo ./realwmediumd/realwmediumd -c tests/dataflow_smoke.<tmp>.cfg
```

脚本会自动生成临时配置文件，内容只有两个接口 ID：

```bash
ifaces :
{
	count = 2;
	ids = ["02:00:00:00:00:00", "02:00:00:00:01:00" ];
};
```

6. 发起 ping 验证。

```bash
ping -I 10.10.10.10 -c 5 -W 1 10.10.10.11
```

## 失败时看什么

1. 查看脚本日志。

```bash
tail -n 80 tests/dataflow_smoke.log
```

2. 检查 realwmediumd 是否启动成功。

```bash
ps -ef | grep realwmediumd | grep -v grep
```

3. 检查 hwsim 接口是否生成。

```bash
ls /sys/class/ieee80211
```

4. 检查路由和规则是否正确。

```bash
ip rule show
ip route show table 266
ip route show table 267
```

## 预期结果

1. `realwmediumd` 能正常启动并保持运行。
2. `ping` 成功返回，不出现丢包。
3. `tests/dataflow_smoke.log` 中没有启动错误或硬件初始化失败信息。