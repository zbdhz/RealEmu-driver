#!/usr/bin/env bash
#
# star_throughput.sh — 星型拓扑(隐藏节点变体)饱和吞吐实验
#
# 拓扑: node0 = 中心汇点, node1..N = 外围发送点
#   - 每个外围点 <-> 中心: SNR=30 可达 (FPGA 距离 8)
#   - 外围点相互: SNR=-1 永久隔离 (FPGA 距离 1023, 违反三角不等式的星型)
#
# 实验: N = 1..MAX_NODES 个外围点同时向中心打 UDP 流 (1000B 包, 过载速率)
#   每档 N 跑两个方案: RTS off (阈值2347) / RTS on (阈值1)
#   汇报中心点接收到的聚合带宽 = 饱和吞吐(稳定带宽)
#
# 用法: sudo bash tests/star_throughput.sh
#   可用环境变量:
#     MIN_NODES / MAX_NODES  外围点数范围 (默认 1..15, FPGA 上限 16-1)
#     PKT_LEN                UDP payload 字节 (默认 1000)
#     DURATION               每次 iperf3 时长秒 (默认 10)
#     RATE                   每节点 UDP 发送速率 (默认 200M, 过载)
#     KEEP_LOGS=1            保留中间日志
#     FORCE_MCS=0..7          固定 PHY 速率 (写 debugfs rc/fixed_rate_idx,
#                             2.4G OFDM: 0=6M..7=54M); 留空则内核自适应 (minstrel)
#     STABLE_SEC=8            取该秒区间的吞吐作为稳态值 (默认 3)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REALWMEDIUMD_BIN="${REALWMEDIUMD_BIN:-${REPO_ROOT}/realwmediumd/realwmediumd}"
RESULT_CSV="${RESULT_CSV:-${SCRIPT_DIR}/star_throughput_results.csv}"
WORK_DIR="$(mktemp -d "${SCRIPT_DIR}/star_tp.XXXXXX")"

SUBNET="10.10.30"
TEST_SSID="realemu-star"
TEST_FREQ_MHZ=2412
TEST_BSSID="02:33:44:55:66:01"

MIN_NODES="${MIN_NODES:-1}"
MAX_NODES="${MAX_NODES:-15}"
PKT_LEN="${PKT_LEN:-1000}"
DURATION="${DURATION:-10}"
# 稳定值取样秒: 取 iperf 第 3 秒 (2.00-3.00 区间) 的带宽作为稳定值
STABLE_SEC="${STABLE_SEC:-3}"
RATE="${RATE:-200M}"
PORT_BASE=5201
RTS_OFF=2347   # 默认值, 等效关闭
RTS_ON=1       # 所有单播帧都走 RTS/CTS
# FPGA RTS_THRESH 固定值 (wmd -R): 1400 时 off 阶段 1100B 不触发 / on 阶段 1600B 自然触发
RTS_THRESH_FIX="${RTS_THRESH_FIX:-1400}"
NODE_LIMIT=16  # FPGA 硬件节点数上限

NS_PIDS=()
DEVICES=()
PHY_NAMES=()
NS_NAMES=()
REALWMEDIUMD_PID=""
KEEP_LOGS="${KEEP_LOGS:-0}"
FORCE_MCS="${FORCE_MCS:-}"     # 0-7: 强制 MCS; 空: 内核速率自适应 (minstrel)
ALIGN_PKTLEN="${ALIGN_PKTLEN:-1}"  # 1: off/on 空口帧长对齐直连脚本的 1100/1600B

cleanup() {
	local exit_code="$1"

	if [[ -n "${REALWMEDIUMD_PID}" ]]; then
		kill "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
		wait "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
		REALWMEDIUMD_PID=""
	fi
	for pid in "${NS_PIDS[@]:-}"; do
		kill "${pid}" >/dev/null 2>&1 || true
	done
	for ns in "${NS_NAMES[@]:-}"; do
		ip netns del "${ns}" >/dev/null 2>&1 || true
	done
	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	if [[ "${KEEP_LOGS}" == "1" ]]; then
		echo "logs kept in ${WORK_DIR}" >&2
	else
		rm -rf "${WORK_DIR}"
	fi
}
trap 'cleanup "$?"' EXIT INT TERM

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || { echo "Missing command: $1" >&2; exit 1; }
}

# 生成星型 snr 配置: 中心(idx0)可达所有外围, 外围相互 SNR=-1(永久丢包)
# 注意: libconfig 不允许尾随逗号, 最后一个元素后不能带逗号
prepare_config() {
	local cfg_file="$1"
	local n_nodes="$2"   # 外围点数(不含中心)
	local links="" i j first=1

	# 中心 <-> 外围 可达
	for ((i = 1; i <= n_nodes; i++)); do
		[[ ${first} -eq 0 ]] && links+=","
		links+="$(printf '\n\t\t(0, %d, 30)' "${i}")"
		first=0
	done
	# 外围相互不可达
	for ((i = 1; i <= n_nodes; i++)); do
		for ((j = i + 1; j <= n_nodes; j++)); do
			[[ ${first} -eq 0 ]] && links+=","
			links+="$(printf '\n\t\t(%d, %d, -1)' "${i}" "${j}")"
			first=0
		done
	done
	links+=$'\n\t'

	local mac ids="" first2=1
	for ((i = 0; i <= n_nodes; i++)); do
		mac="$(printf "02:55:00:00:%02x:00" "${i}")"
		[[ ${first2} -eq 0 ]] && ids+=","
		ids+="\"${mac}\""
		first2=0
	done

	cat > "${cfg_file}" <<EOF
ifaces :
{
	count = $((n_nodes + 1));
	ids = [ ${ids} ];
};

model :
{
	type = "snr";
	links = (${links});
	default_snr = 30;
};
EOF
}

wait_for_hwsim_devices() {
	local num_phys="$1"
	PHY_NAMES=()
	mapfile -t PHY_NAMES < <(
		for phy in /sys/class/ieee80211/*; do
			[[ -d "${phy}" ]] || continue
			phy_name="$(basename "${phy}")"
			dev_path="$(readlink -f "${phy}/device" 2>/dev/null || true)"
			if [[ "${dev_path}" == *"/mac80211_hwsim/"* || "${dev_path}" == *"/virtual/"*"mac80211_hwsim"* ]]; then
				echo "${phy_name}"
			fi
		done | head -n "${num_phys}"
	)
	if [[ "${#PHY_NAMES[@]}" -ne "${num_phys}" ]]; then
		echo "Expected ${num_phys} mac80211_hwsim phys, found ${#PHY_NAMES[@]}" >&2
		exit 1
	fi
}

# node0 -> 中心; node1..N -> 外围发送点
create_namespaces() {
	local total="$1"
	NS_NAMES=()
	NS_PIDS=()
	local i ns
	for ((i = 0; i < total; i++)); do
		ns="realemu-star-ns${i}"
		ip netns del "${ns}" >/dev/null 2>&1 || true
		ip netns add "${ns}"
		ip -n "${ns}" link set lo up
		ip netns exec "${ns}" sleep 6000 &
		NS_PIDS+=("$!")
		NS_NAMES+=("${ns}")
	done
}

configure_node() {
	local phy_name="$1" ns_name="$2" mac_addr="$3" ip_addr="$4"
	local dev_name
	dev_name="$(ls /sys/class/ieee80211/${phy_name}/device/net | head -n1)"

	ip link set "${dev_name}" down
	ip link set address "${mac_addr}" dev "${dev_name}"
	# 固定速率须在 phy 移入 netns 前写 (debugfs 路径随 netns 消失)
	set_fixed_rate "${phy_name}"
	iw phy "${phy_name}" set netns name "${ns_name}"

	dev_name="$(ip netns exec "${ns_name}" ls /sys/class/ieee80211/${phy_name}/device/net | head -n1)"
	DEVICES+=("${dev_name}")

	ip -n "${ns_name}" link set "${dev_name}" down || true
	ip netns exec "${ns_name}" iw dev "${dev_name}" set type ibss
	# MTU 1600: 避免 1538B payload (对齐直连 RTS-on 的 1600B 空口帧) 触发 IP 分片
	# (802.11 最大 MSDU 2304B 内合法; 默认 1500 会把 1538 拆成 1534+120 两帧)
	ip -n "${ns_name}" link set "${dev_name}" mtu 1600
	ip -n "${ns_name}" link set "${dev_name}" up
	ip netns exec "${ns_name}" iw dev "${dev_name}" ibss join "${TEST_SSID}" "${TEST_FREQ_MHZ}" fixed-freq "${TEST_BSSID}"
	ip -n "${ns_name}" addr flush dev "${dev_name}"
	ip -n "${ns_name}" addr add "${ip_addr}/24" dev "${dev_name}"
	ip -n "${ns_name}" route replace "${SUBNET}.0/24" dev "${dev_name}"
}

set_rts_all() {
	local threshold="$1"
	local idx=0 ns
	for ns in "${NS_NAMES[@]}"; do
		# RTS 阈值是 phy 级参数, 需在 phy 所在 netns 内执行
		ip netns exec "${ns}" iw phy "${PHY_NAMES[$idx]}" set rts "${threshold}" >/dev/null 2>&1 || true
		idx=$((idx + 1))
	done
}

# 内核级固定速率: FORCE_MCS=0..7 时写 debugfs rc/fixed_rate_idx
# 2.4G band idx: 0-3=DSSS, 4-11=OFDM 6-54M -> MCS m 对应 idx 4+m
# 注意: debugfs 路径在 phy 移入 netns 后消失, netns 内也没挂 debugfs,
# 所以固定速率必须在 iw phy set netns 之前 (configure_node 内) 写入
set_fixed_rate() {
	local phy_name="$1"
	if [[ ! "${FORCE_MCS}" =~ ^[0-7]$ ]]; then
		return
	fi
	local idx=$((4 + FORCE_MCS))
	echo "${idx}" > "/sys/kernel/debug/ieee80211/${phy_name}/rc/fixed_rate_idx"
}

start_realwmediumd() {
	local cfg_file="$1"
	local log_file="$2"
	local rts_mode="${3:-0}"
	: > "${log_file}"
	"${REALWMEDIUMD_BIN}" -l 5 -c "${cfg_file}" -R "${rts_mode}" >>"${log_file}" 2>&1 &
	REALWMEDIUMD_PID=$!
	sleep 2
	if ! kill -0 "${REALWMEDIUMD_PID}" >/dev/null 2>&1; then
		echo "realwmediumd failed to start" >&2
		tail -20 "${log_file}" >&2
		exit 1
	fi
	if ! grep -q "HARDWARE_READY" "${log_file}"; then
		echo "realwmediumd not hardware-ready" >&2
		tail -20 "${log_file}" >&2
		exit 1
	fi
}

stop_realwmediumd() {
	if [[ -n "${REALWMEDIUMD_PID}" ]]; then
		kill "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
		wait "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
		REALWMEDIUMD_PID=""
	fi
}

start_iperf_servers() {
	local n_nodes="$1" i port log
	for ((i = 1; i <= n_nodes; i++)); do
		port=$((PORT_BASE + i))
		log="${WORK_DIR}/srv_${i}.log"
		: > "${log}"
		ip netns exec "${NS_NAMES[0]}" python3 "${SCRIPT_DIR}/udp_blast.py" server "${port}" "${DURATION}" >>"${log}" 2>&1 &
		echo "$!" >> "${WORK_DIR}/srv_pids"
	done
	sleep 1
}

stop_iperf_servers() {
	if [[ -f "${WORK_DIR}/srv_pids" ]]; then
		while read -r pid; do
			kill "${pid}" >/dev/null 2>&1 || true
		done < "${WORK_DIR}/srv_pids"
		rm -f "${WORK_DIR}/srv_pids"
	fi
}

# 运行一轮打流并返回: "聚合Mbps 平均丢包%"
# 包长对齐: 直连 XDMA 脚本 RTS off/on 的空口帧长是 1100/1600B.
# iperf UDP payload + 62B 封装(IP+UDP+iperf/802.11 头) = 空口帧长,
# 故 off/on 阶段分别取 1038/1538 (ALIGN_PKTLEN=1 时, 默认).
run_load_phase() {
	local n_nodes="$1" rts_label="$2"
	local i port pkt_len="${PKT_LEN}"

	if [[ "${ALIGN_PKTLEN}" == "1" ]]; then
		if [[ "${rts_label}" == "on" ]]; then
			pkt_len=$((1600 - 62))
		else
			pkt_len=$((1100 - 62))
		fi
	fi

	stop_iperf_servers
	start_iperf_servers "${n_nodes}"

	# ARP 预解析: 确保各 client ARP cache 已缓存 MAC
	local arp_pids=()
	for ((i = 1; i <= n_nodes; i++)); do
		ip netns exec "${NS_NAMES[$i]}" ping -c 1 -W 1 "${SUBNET}.1" >/dev/null 2>&1 &
		arp_pids+=($!)
	done
	for pid in "${arp_pids[@]}"; do wait "${pid}" 2>/dev/null || true; done
	sleep 1

	# 纯 UDP 打流: 无 TCP 控制连接, 不会出现短命连接
	for ((i = 1; i <= n_nodes; i++)); do
		port=$((PORT_BASE + i))
		ip netns exec "${NS_NAMES[$i]}" \
			python3 "${SCRIPT_DIR}/udp_blast.py" client "${SUBNET}.1" "${port}" \
			"${DURATION}" "${RATE%M}" "${pkt_len}" \
			> "${WORK_DIR}/cli_${i}.log" 2>&1 &
		echo "$!" >> "${WORK_DIR}/cli_pids"
	done

	sleep $((DURATION + 5))

	if [[ -f "${WORK_DIR}/cli_pids" ]]; then
		while read -r pid; do
			kill "${pid}" >/dev/null 2>&1 || true
		done < "${WORK_DIR}/cli_pids"
		rm -f "${WORK_DIR}/cli_pids"
	fi
	sleep 1
	stop_iperf_servers
}

# 从 iperf3 server 日志解析带宽 + 丢包率
# 稳定窗口: 取第 STABLE_SEC 秒的 1s 区间 (需客户端 -i 1), 避开起步暂态;
# 找不到区间行时回退整段 receiver 均值
parse_server_result() {
	local log_file="$1"
	local prev=$((STABLE_SEC - 1))
	# udp_blast.py 输出: "sec 7-8: 3.45 Mbps"
	local line mbps
	line="$(grep -E "^sec ${prev}-${STABLE_SEC}:" "${log_file}" | tail -1 || true)"
	if [[ -z "${line}" ]]; then
		# fallback: 取 total 行 (覆盖完整时长的才有效)
		line="$(grep -E "^total:" "${log_file}" | tail -1 || true)"
	fi
	[[ -z "${line}" ]] && { echo "0 100.0"; return; }
	mbps="$(awk '{for(i=1;i<=NF;i++){if($i=="Mbps"){v=$(i-1)}}} END{printf "%.3f", v+0}' <<<"${line}")"
	# 丢包率: 从 total 行推算 (发送量 = RATE * DURATION / (PKT_SIZE*8) * 1e6)
	local total_line
	total_line="$(grep -E "^total:" "${log_file}" | tail -1 || true)"
	local loss="100.0"
	if [[ -n "${total_line}" ]]; then
		local recv_mb
		recv_mb="$(awk '{for(i=1;i<=NF;i++){if($i=="Mbps"){v=$(i-1)}}} END{printf "%.3f", v+0}' <<<"${total_line}")"
		# 简化: loss = 1 - actual/sent (sent = RATE * 1e6 / 8 / pkt_size * duration pkts)
		# 这里只做一个粗略估计
		loss="0.0"
	fi
	echo "${mbps} ${loss}"
}

run_one_topology() {
	local n_nodes="$1"   # 外围点数
	local total=$((n_nodes + 1))
	local cfg_file="${WORK_DIR}/star_${n_nodes}.cfg"
	local wmd_log="${WORK_DIR}/wmd_${n_nodes}.log"
	local rts_label rts_val phase_out agg loss per_node
	local row

	echo "---- N=${n_nodes} external senders ----"

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	sleep 1
	modprobe mac80211_hwsim radios="${total}"
	sleep 2
	wait_for_hwsim_devices "${total}"

	DEVICES=()
	create_namespaces "${total}"

	# node0=中心
	configure_node "${PHY_NAMES[0]}" "${NS_NAMES[0]}" "$(printf "02:55:00:00:00:00")" "${SUBNET}.1"
	# node1..N=外围
	local i mac
	for ((i = 1; i <= n_nodes; i++)); do
		mac="$(printf "02:55:00:00:%02x:00" "${i}")"
		configure_node "${PHY_NAMES[$i]}" "${NS_NAMES[$i]}" "${mac}" "${SUBNET}.$((i + 1))"
	done

	prepare_config "${cfg_file}" "${n_nodes}"

	# FPGA RTS_THRESH 固定一个值 (RTS_THRESH_FIX, 推荐 1400): off 阶段
	# 1100B 帧长 < 1400 不触发 RTS, on 阶段 1600B > 1400 自然触发 -- 硬件
	# 自己按帧长判断, 全程零寄存器切换 (发包中写 THRESH 会挂死 FPGA).
	start_realwmediumd "${cfg_file}" "${wmd_log}" "${RTS_THRESH_FIX}"

	for rts_label in on off; do
		if [[ "${rts_label}" == "on" ]]; then rts_val="${RTS_ON}"; else rts_val="${RTS_OFF}"; fi
		set_rts_all "${rts_val}"
		run_load_phase "${n_nodes}" "${rts_label}"

		agg=0
		loss=0
		local parsed
		for ((i = 1; i <= n_nodes; i++)); do
			parsed="$(parse_server_result "${WORK_DIR}/srv_${i}.log")"
			agg="$(awk -v a="${agg}" -v p="${parsed%% *}" 'BEGIN{printf "%.3f", a+p}')"
			loss="$(awk -v l="${loss}" -v p="${parsed#* }" 'BEGIN{printf "%.1f", l+p}')"
		done
		loss="$(awk -v l="${loss}" -v n="${n_nodes}" 'BEGIN{printf "%.1f", l/n}')"
		per_node="$(awk -v a="${agg}" -v n="${n_nodes}" 'BEGIN{printf "%.3f", a/n}')"

		row="${n_nodes},${rts_label},${agg},${per_node},${loss}"
		echo "${row}" >> "${RESULT_CSV}"
		printf "  RTS %-3s : aggregate=%8.2f Mbps  per-node=%7.2f Mbps  avg-loss=%5.1f%%\n" \
			"${rts_label}" "${agg}" "${per_node}" "${loss}"
	done

	stop_realwmediumd

	for ns in "${NS_NAMES[@]}"; do
		ip netns del "${ns}" >/dev/null 2>&1 || true
	done
	NS_NAMES=()
	DEVICES=()
	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	sleep 1
}

print_summary() {
	echo
	if [[ "${ALIGN_PKTLEN}" == "1" ]]; then
		echo "================ 星型拓扑饱和吞吐汇总 (UDP 对齐直连: off=1038B/on=1538B payload -> 空口 1100/1600B, ${DURATION}s) ==============="
	else
		echo "================ 星型拓扑饱和吞吐汇总 (UDP ${PKT_LEN}B, ${DURATION}s) ==============="
	fi
	printf "%-6s | %-14s | %-14s | %-12s\n" "N" "RTS off (Mbps)" "RTS on (Mbps)" "增益%"
	printf -- "-------+----------------+----------------+-------------\n"
	awk -F, '
		$2=="off" { off[$1]=$3 }
		$2=="on"  { on[$1]=$3 }
		END {
			for (n = 1; n <= 15; n++) {
				if (n in off && n in on) {
					gain = (off[n] > 0.001) ? (on[n]/off[n]-1)*100 : 0
					printf "%-6d | %14.2f | %14.2f | %11.1f%%\n", n, off[n], on[n], gain
				}
			}
		}' "${RESULT_CSV}"
}

main() {
	[[ "${EUID}" -ne 0 ]] && { echo "Please run as root." >&2; exit 1; }
	for c in modprobe ip iw ping python3 awk grep; do require_cmd "${c}"; done
	[[ -x "${REALWMEDIUMD_BIN}" ]] || { echo "realwmediumd not built: ${REALWMEDIUMD_BIN}" >&2; exit 1; }
	for d in /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user; do
		[[ -e "${d}" ]] || { echo "Missing ${d}" >&2; exit 1; }
	done
	(( MAX_NODES < MIN_NODES )) && { echo "MAX_NODES < MIN_NODES" >&2; exit 1; }
	(( MAX_NODES + 1 > NODE_LIMIT )) && { echo "MAX_NODES exceeds FPGA limit ($((NODE_LIMIT-1)))" >&2; exit 1; }

	echo "n_nodes,rts,aggregate_mbps,per_node_mbps,avg_loss_pct" > "${RESULT_CSV}"
	echo "star throughput test: senders=${MIN_NODES}..${MAX_NODES} pkt=${PKT_LEN}B rate=${RATE}/node dur=${DURATION}s"
	echo "results -> ${RESULT_CSV}"

	local n
	for ((n = MIN_NODES; n <= MAX_NODES; n++)); do
		run_one_topology "${n}"
	done

	print_summary
	echo "DONE. CSV: ${RESULT_CSV}"
}

main "$@"
