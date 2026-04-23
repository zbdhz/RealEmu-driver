#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REALWMEDIUMD_BIN="${REALWMEDIUMD_BIN:-${REPO_ROOT}/realwmediumd/realwmediumd}"
LOG_FILE="${LOG_FILE:-${SCRIPT_DIR}/dataflow_smoke.log}"
WMEDIUMD_LOG_FILE="${WMEDIUMD_LOG_FILE:-${SCRIPT_DIR}/realwmediumd_smoke.log}"
WORK_DIR="$(mktemp -d "${SCRIPT_DIR}/dataflow_smoke.XXXXXX")"
CFG_FILE="${WORK_DIR}/dataflow_smoke.cfg"

SUBNET="10.10.10"
NUM_PHYS=2
TEST_SSID="realemu-test"
TEST_FREQ_MHZ=2412
TEST_BSSID="02:11:22:33:44:55"
RADIO_MACS=(
	"02:00:00:00:00:00"
	"02:00:00:00:01:00"
)
NS_NAMES=(
	"realemu-ns0"
	"realemu-ns1"
)

NS_PIDS=()

DEVICES=()
PHY_NAMES=()
REALWMEDIUMD_PID=""

cleanup() {
	local exit_code="$1"

	for dev in "${DEVICES[@]:-}"; do
		ip link set "${dev}" down >/dev/null 2>&1 || true
	done

	if [[ -n "${REALWMEDIUMD_PID}" ]]; then
		kill "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
		wait "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || true
	fi

	# kill any background namespace helper processes
	for pid in "${NS_PIDS[@]:-}"; do
		kill "${pid}" >/dev/null 2>&1 || true
	done

	for ns in "${NS_NAMES[@]}"; do
		ip netns del "${ns}" >/dev/null 2>&1 || true
	done

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	rm -rf "${WORK_DIR}"

	if [[ "${exit_code}" -ne 0 && -f "${WMEDIUMD_LOG_FILE}" ]]; then
		echo "realwmediumd log tail:"
		tail -n 80 "${WMEDIUMD_LOG_FILE}" || true
	fi
}

trap 'cleanup "$?"' EXIT INT TERM

require_cmd() {
	local cmd_name="$1"
	if ! command -v "${cmd_name}" >/dev/null 2>&1; then
		echo "Missing required command: ${cmd_name}" >&2
		exit 1
	fi
}

require_root() {
	if [[ "${EUID}" -ne 0 ]]; then
		echo "Please run this test as root." >&2
		exit 1
	fi
}

prepare_config() {
	cat > "${CFG_FILE}" <<EOF
ifaces :
{
	count = 2;
	ids = ["${RADIO_MACS[0]}", "${RADIO_MACS[1]}" ];
};

model :
{
	type = "snr";
	links = ( );
	default_snr = 30;
};
EOF
}

wait_for_hwsim_devices() {
	PHY_NAMES=()
	mapfile -t PHY_NAMES < <(
		for phy in /sys/class/ieee80211/*; do
			[[ -d "${phy}" ]] || continue
			phy_name="$(basename "${phy}")"
			dev_path="$(readlink -f "${phy}/device" 2>/dev/null || true)"
			if [[ "${dev_path}" == *"/mac80211_hwsim/"* || "${dev_path}" == *"/virtual/"*"mac80211_hwsim"* ]]; then
				echo "${phy_name}"
			fi
		done | head -n "${NUM_PHYS}"
	)
	if [[ "${#PHY_NAMES[@]}" -ne "${NUM_PHYS}" ]]; then
		echo "Expected ${NUM_PHYS} mac80211_hwsim phys, found ${#PHY_NAMES[@]}" >&2
		exit 1
	fi
}

create_namespaces() {
	NS_PIDS=()
	for ns in "${NS_NAMES[@]}"; do
		ip netns del "${ns}" >/dev/null 2>&1 || true
		ip netns add "${ns}"
		ip -n "${ns}" link set lo up
		# start a sleeping helper inside namespace so we have a PID to move devices to
		ip netns exec "${ns}" sleep 6000 &
		NS_PIDS+=("$!")
	done
}

configure_station() {
	local phy_name="$1"
	local ns_name="$2"
	local mac_addr="$3"
	local ip_addr="$4"

	local dev_name
	dev_name="$(ls /sys/class/ieee80211/${phy_name}/device/net | head -n1)"

	echo "Configuring ${dev_name} (phy: ${phy_name}, netns: ${ns_name})..."

	ip link set "${dev_name}" down || { echo "Failed to set ${dev_name} down"; exit 1; }
	ip link set address "${mac_addr}" dev "${dev_name}" || { echo "Failed to set MAC ${mac_addr} on ${dev_name}"; exit 1; }
	iw phy "${phy_name}" set netns name "${ns_name}" || {
		echo "Failed to move phy ${phy_name} to ${ns_name}" >&2
		exit 1
	}

	dev_name="$(ip netns exec "${ns_name}" ls /sys/class/ieee80211/${phy_name}/device/net | head -n1)"
	DEVICES+=("${dev_name}")

	ip -n "${ns_name}" link set "${dev_name}" down || true
	ip netns exec "${ns_name}" iw dev "${dev_name}" set type ibss || { echo "Failed to set ${dev_name} to ibss mode (netns)"; exit 1; }
	ip -n "${ns_name}" link set "${dev_name}" up || { echo "Failed to bring up ${dev_name} in ${ns_name}"; exit 1; }
	ip netns exec "${ns_name}" iw dev "${dev_name}" ibss join "${TEST_SSID}" "${TEST_FREQ_MHZ}" fixed-freq "${TEST_BSSID}" || {
		echo "Failed to join IBSS on ${dev_name} in ${ns_name}"; exit 1;
	}
	ip -n "${ns_name}" addr flush dev "${dev_name}"
	ip -n "${ns_name}" addr add "${ip_addr}/24" dev "${dev_name}"
	ip -n "${ns_name}" route replace "${SUBNET}.0/24" dev "${dev_name}"
}

assert_data_plane_activity() {
	local tx_0_to_1
	local tx_1_to_0
	local hw_complete

	tx_0_to_1=$(grep -c "\\[TX\\] Frame queued to TX queue: 0->1" "${WMEDIUMD_LOG_FILE}" || true)
	tx_1_to_0=$(grep -c "\\[TX\\] Frame queued to TX queue: 1->0" "${WMEDIUMD_LOG_FILE}" || true)
	hw_complete=$(grep -c "\\[COMPLETE\\] Frame cookie=.*ACKed by hardware" "${WMEDIUMD_LOG_FILE}" || true)

	echo "TX 0->1 count: ${tx_0_to_1}"
	echo "TX 1->0 count: ${tx_1_to_0}"
	echo "HW complete count: ${hw_complete}"

	if [[ "${tx_0_to_1}" -eq 0 || "${tx_1_to_0}" -eq 0 || "${hw_complete}" -eq 0 ]]; then
		echo "FAIL: data plane activity missing in realwmediumd log" >&2
		echo "Expected at least one 0->1 TX, one 1->0 TX, and one hardware completion." >&2
		return 1
	fi
}

main() {
	require_root
	require_cmd modprobe
	require_cmd ip
	require_cmd iw
	require_cmd ping
	require_cmd ls
	require_cmd tail
	require_cmd grep

	if [[ ! -x "${REALWMEDIUMD_BIN}" ]]; then
		echo "realwmediumd binary not found: ${REALWMEDIUMD_BIN}" >&2
		exit 1
	fi

	for device_path in /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user; do
		if [[ ! -e "${device_path}" ]]; then
			echo "Missing XDMA device: ${device_path}" >&2
			exit 1
		fi
	done

	prepare_config
	: > "${LOG_FILE}"
	: > "${WMEDIUMD_LOG_FILE}"

	# 验证生成的配置文件
	echo "=== Generated config file ===" >> "${LOG_FILE}"
	cat "${CFG_FILE}" >> "${LOG_FILE}"
	echo "=============================" >> "${LOG_FILE}"

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	sleep 1
	modprobe mac80211_hwsim radios="${NUM_PHYS}"
	sleep 1

	# 验证hwsim模块是否正确加载
	if ! ls /sys/class/ieee80211 >/dev/null 2>&1; then
		echo "Error: mac80211_hwsim module failed to load" >&2
		exit 1
	fi

	wait_for_hwsim_devices
	create_namespaces

	configure_station "${PHY_NAMES[0]}" "${NS_NAMES[0]}" "${RADIO_MACS[0]}" "${SUBNET}.10"
	configure_station "${PHY_NAMES[1]}" "${NS_NAMES[1]}" "${RADIO_MACS[1]}" "${SUBNET}.11"

	"${REALWMEDIUMD_BIN}" -l 7 -c "${CFG_FILE}" >>"${WMEDIUMD_LOG_FILE}" 2>&1 &
	REALWMEDIUMD_PID=$!
	sleep 2

	if ! kill -0 "${REALWMEDIUMD_PID}" >/dev/null 2>&1; then
		echo "realwmediumd failed to start" >&2
		echo "=== realwmediumd log ===" >&2
		cat "${WMEDIUMD_LOG_FILE}" >&2
		exit 1
	fi

	echo "realwmediumd started with PID ${REALWMEDIUMD_PID}"

	ip netns exec "${NS_NAMES[0]}" ping -c 5 -W 1 "${SUBNET}.11"

	assert_data_plane_activity

	echo "=== realwmediumd log tail ==="
	tail -n 80 "${WMEDIUMD_LOG_FILE}" || true

	echo "PASS: data flow smoke test completed successfully"
	echo "Script log file: ${LOG_FILE}"
	echo "realwmediumd log file: ${WMEDIUMD_LOG_FILE}"
}

main "$@"