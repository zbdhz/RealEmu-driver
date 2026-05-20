# MININET-WIFI 与 RealEmu-driver 最小化文件级改造清单

说明：本清单严格遵循“仅修改 mininet-wifi 必要交互面；仅在 RealEmu-driver 修改顶层 .c/.h（如确有必要）”的原则。其它驱动内部逻辑不改动，除非在清单中特别标注为“必须修改”。

**目标**：使 realwmediumd 可被 Mininet-WiFi 当作与 wmediumd 同级的顶层后端进行调用与控制（启动、读取日志/状态、位置更新、优雅退出），最小化跨仓库改动。

**总体思路**：
- 在 `mininet-wifi` 中，提供一个与现有 `wmediumdConnector` API 兼容（或充分相似）的 `realemuConnector`，并在 Mininet 的后端选择路径（`net.py`/`link.py`）处增加最小分支以支持 `realwmediumd`。
- 在 `RealEmu-driver` 中，只对 `realwmediumd` 的顶层可执行/头文件接口做必要微调：保证命令行选项和日志格式与 `wmediumd`/mininet-wifi 期待的交互点一致（如 `-c <config>`、可指定日志文件、以及确定的“硬件就绪/回退”打印消息）。

**改造清单（按仓库、文件列出，最小化）**

**mininet-wifi（必须修改，按优先级）**
- **文件：mn_wifi/realemuConnector.py**
	- 改动概要：将现有实现调整为与 `mn_wifi/wmediumdConnector.py` 的对外 API 兼容。
	- 具体要点：
		- 提供 `start()`（启动 realwmediumd 进程并返回后端句柄），`register_interface(intf)`，`update_pos(intf, x,y,z)`，`shutdown()` 等方法。
		- 暴露 `wmd_config_name` 与 `wmd_logfile` 或等价属性（供 Mininet 生成/传入配置与日志路径）。
		- 在启动后把后端控制句柄或兼容替身放到 Mininet 的可访问位置，供 `node.py`、`mobility.py` 调用位置更新。 
	- 为什么改：最小化上层调用方改动，直接替换后端实现即可。

- **文件：mn_wifi/link.py**
	- 改动概要：在链路配置处增加对 `realwmediumd` 的最小分支。
	- 具体要点：
		- 新增 `configRealemu(intfrefs, **kwargs)` 或让 `configWmediumd()` 接受 `backend='realemu'` 参数并据此构造 `realemuConnector` 的启动参数。
		- 确保 `intfrefs`（接口集合）以 `realemuConnector` 可读格式传递（MAC、ifname、node id 等）。
	- 为什么改：让链路生成与后端启动器协作，收集必要接口描述。

- **文件：mn_wifi/net.py**
	- 改动概要：在现有 `start_wmediumd()`/`init_wmediumd()` 周边增加 minimal 分支或扩展以支持 `realwmediumd` 模式。
	- 具体要点：
		- 在后端选择逻辑中识别 `wmediumd_mode == 'realwmediumd'`（或接受 `--link=realwmediumd`），并调用 `realemuConnector.start()` 来启动后端。
		- 将返回的后端句柄或委派接口放到全局可访问位置，供 `node.py` 等调用。
	- 为什么改：把后端启动的控制权移交给 `realemuConnector`，并保持 Mininet 的拓扑/生命周期一致。

- **文件：mn_wifi/node.py**（小范围改动）
	- 改动概要：把位置更新调用集中到一个委派点。
	- 具体要点：
		- 修改 `set_pos_wmediumd()`（或相关函数）以调用统一的 `wmediumd_update_pos(mac, x,y,z)` 去路由给当前后端（wmediumd 或 realwmediumd）。
		- 若已有全局 `w_server`，令 `realemuConnector` 在启动时将其兼容替身注入，避免多处条件判断。
	- 为什么改：单点处理位置更新，最小化对 `propagationModels.py`/`mobility.py` 的改动。

- **文件：mn_wifi/propagationModels.py 与 mn_wifi/mobility.py**
	- 改动概要：把对 `w_server` 的直接调用替换为对统一委派接口的调用（或保持不变，如果 `realemuConnector` 注入兼容的 `w_server`）。
	- 为什么改：保证位置/传播更新在使用 realwmediumd 时仍然有效，且避免广泛修改。

- **文件：mn_wifi/clean.py**
	- 改动概要：在清理流程加入对 `realwmediumd` 的优雅停止调用。
	- 具体要点：调用 `realemuConnector.shutdown()`；若没响应再 fallback 到 `pkill realwmediumd`。

**RealEmu-driver（仅在必要时修改顶层文件）**
- **文件：realwmediumd/realwmediumd.c**（必要时修改）
	- 必要改动（最小化）：
		- 支持接收 `-c <config>` 参数或等价配置文件路径。
		- 支持指定日志输出路径（或至少保证 stdout 输出可被 Mininet 重定向解析）。
		- 在启动时打印标准化日志标记，便于 Mininet 在启动阶段通过日志识别“硬件就绪”或“软件回退”状态（例如：`REALW: HARDWARE_READY` / `REALW: SOFTWARE_FALLBACK`）。
	- 为什么改：Mininet 在硬件/软件模式判定时需要可解析的信号；通过最小化 CLI/日志改动即可让 Mininet 可靠检测运行状态。

- **文件：realwmediumd/realwmediumd_dynamic.h 或 顶层头文件（仅在需要时）**
	- 必要改动（条件性的）：
		- 若 Mininet/connector 需要直接链接到 driver 导出的函数或符号，则在顶层头文件中提供与 `wmediumd` 相兼容的最小 wrapper（导出相同名称/签名），否则无需修改。

**何时需要对 RealEmu-driver 做额外改动（仅在检测出不兼容时）**
- 如果现有 `realwmediumd`：
	- 无法通过 `-c` 指定配置文件；或
	- 不支持将日志输出到指定文件或 stdout 中不包含可解析就绪标记；或
	- 控制接口（socket/netlink/wserver）与 `wmediumd` 完全不兼容且 connector 不能通过进程层面封装弥补；
 以上任一情况，才在 `realwmediumd` 的顶层 `.c/.h` 中做小范围改动以暴露兼容行为。

**优先级与验证步骤（建议）**
- 第一步（高优先级）：在 `mininet-wifi` 中完成 `realemuConnector.py` 的 API 对齐，并在 `net.py`/`link.py` 中添加最小分支；此步骤完成后，可在“软件回退”环境下运行示例验证行为。
- 第二步（中优先级）：对 `realwmediumd.c` 做最小 CLI/日志强化（如果当前可执行不能被 Mininet 可靠检测），并运行 `tests/distance_smoke.sh` 风格的脚本验证日志输出包含预期标记。
- 第三步（低优先级）：如需要再调整 `realwmediumd` 的顶层导出符号以匹配 `wmediumd` 的外部控制接口。

**备注（为何是最小化改动）**
- 所有 mininet-wifi 的改动集中在“后端交互面”：启动、位置更新、日志就绪检测、优雅退出。这样能把兼容性范围限制在少数文件且便于回滚。
- RealEmu-driver 内部无线调度/硬件控制逻辑不修改，除非顶层接口（CLI/日志/控制 socket）不满足上述交互需求；只有在发现不兼容时才在顶层 .c/.h 做小范围改动。
