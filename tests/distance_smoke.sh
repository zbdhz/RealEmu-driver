#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REALWMEDIUMD_BIN="${REALWMEDIUMD_BIN:-${REPO_ROOT}/realwmediumd/realwmediumd}"
LOG_FILE="${LOG_FILE:-${SCRIPT_DIR}/distance_smoke.log}"
NEAR_LOG_FILE="${NEAR_LOG_FILE:-${SCRIPT_DIR}/distance_near_realwmediumd.log}"
FAR_LOG_FILE="${FAR_LOG_FILE:-${SCRIPT_DIR}/distance_far_realwmediumd.log}"
WORK_DIR="$(mktemp -d "${SCRIPT_DIR}/distance_smoke.XXXXXX")"

SUBNET="10.10.20"
NUM_PHYS=2
TEST_SSID="realemu-distance-test"
TEST_FREQ_MHZ=2412
TEST_BSSID="02:11:22:33:44:66"
RADIO_MACS=(
	"02:00:00:00:10:00"
	"02:00:00:00:11:00"
)
NS_NAMES=(
	"realemu-distance-ns0"
	"realemu-distance-ns1"
)

NEAR_DISTANCE=8
FAR_DISTANCE=1023
PING_COUNT=3
PING_TIMEOUT=1

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

	for pid in "${NS_PIDS[@]:-}"; do
		kill "${pid}" >/dev/null 2>&1 || true
	done

	for ns in "${NS_NAMES[@]}"; do
		ip netns del "${ns}" >/dev/null 2>&1 || true
	done

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	rm -rf "${WORK_DIR}"

	if [[ "${exit_code}" -ne 0 ]]; then
		for file in "${NEAR_LOG_FILE}" "${FAR_LOG_FILE}"; do
			if [[ -f "${file}" ]]; then
				echo "=== tail of ${file} ==="
				tail -n 60 "${file}" || true
			fi
		done
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
	local cfg_file="$1"
	local distance_m="$2"

	cat > "${cfg_file}" <<EOF
ifaces :
{
	count = 2;
	ids = ["${RADIO_MACS[0]}", "${RADIO_MACS[1]}" ];
};

model :
{
	type = "path_loss";
	positions = (
		(0.0, 0.0, 0.0),
		(${distance_m}.0, 0.0, 0.0)
	);
	fading_coefficient = 0;
	noise_threshold = -91;
	isnodeaps = (0, 0);
	tx_powers = (14, 14);
	model_name = "log_distance";
	path_loss_exp = 3.0;
	xg = 0.0;
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

start_realwmediumd() {
	local cfg_file="$1"
	local log_file="$2"

	: > "${log_file}"
	"${REALWMEDIUMD_BIN}" -l 7 -c "${cfg_file}" >>"${log_file}" 2>&1 &
	REALWMEDIUMD_PID=$!
	sleep 2

	if ! kill -0 "${REALWMEDIUMD_PID}" >/dev/null 2>&1; then
		echo "realwmediumd failed to start" >&2
		cat "${log_file}" >&2
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

run_phase() {
	local phase_name="$1"
	local expected_success="$2"
	local cfg_file="$3"
	local log_file="$4"
	local src_ns="$5"
	local dst_ns="$6"
	local dst_ip="$7"
	local distance_m="$8"

	prepare_config "${cfg_file}" "${distance_m}"

	echo "=== ${phase_name} config ===" >>"${LOG_FILE}"
	cat "${cfg_file}" >>"${LOG_FILE}"
	echo "========================" >>"${LOG_FILE}"

	start_realwmediumd "${cfg_file}" "${log_file}"
	echo "${phase_name}: realwmediumd PID ${REALWMEDIUMD_PID}" | tee -a "${LOG_FILE}"

	ip netns exec "${src_ns}" ip neigh flush all >/dev/null 2>&1 || true
	ip netns exec "${dst_ns}" ip neigh flush all >/dev/null 2>&1 || true

	if [[ "${expected_success}" == "yes" ]]; then
		if ! ip netns exec "${src_ns}" ping -c "${PING_COUNT}" -W "${PING_TIMEOUT}" "${dst_ip}"; then
			echo "FAIL: ${phase_name} expected ping success, but ping failed" >&2
			return 1
		fi
		if ! grep -q "\[COMPLETE\] Frame cookie=.*ACKed by hardware" "${log_file}"; then
			echo "FAIL: ${phase_name} did not produce a hardware ACK completion" >&2
			return 1
		fi
	else
		if ip netns exec "${src_ns}" ping -c "${PING_COUNT}" -W "${PING_TIMEOUT}" "${dst_ip}"; then
			echo "FAIL: ${phase_name} expected ping failure at long distance, but ping succeeded" >&2
			return 1
		fi
		if ! grep -q "\[QUEUE\] Frame received" "${log_file}"; then
			echo "FAIL: ${phase_name} did not process any frames in realwmediumd" >&2
			return 1
		fi
	fi

	echo "=== ${phase_name} log tail ===" >>"${LOG_FILE}"
	tail -n 40 "${log_file}" >>"${LOG_FILE}" || true
	stop_realwmediumd
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

	: > "${LOG_FILE}"
	: > "${NEAR_LOG_FILE}"
	: > "${FAR_LOG_FILE}"

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	sleep 1
	modprobe mac80211_hwsim radios="${NUM_PHYS}"
	sleep 1

	if ! ls /sys/class/ieee80211 >/dev/null 2>&1; then
		echo "Error: mac80211_hwsim module failed to load" >&2
		exit 1
	fi

	wait_for_hwsim_devices
	create_namespaces

	configure_station "${PHY_NAMES[0]}" "${NS_NAMES[0]}" "${RADIO_MACS[0]}" "${SUBNET}.10"
	configure_station "${PHY_NAMES[1]}" "${NS_NAMES[1]}" "${RADIO_MACS[1]}" "${SUBNET}.11"

	run_phase "near-distance" yes "${WORK_DIR}/near.cfg" "${NEAR_LOG_FILE}" "${NS_NAMES[0]}" "${NS_NAMES[1]}" "${SUBNET}.11" "${NEAR_DISTANCE}"
	run_phase "far-distance" no "${WORK_DIR}/far.cfg" "${FAR_LOG_FILE}" "${NS_NAMES[0]}" "${NS_NAMES[1]}" "${SUBNET}.11" "${FAR_DISTANCE}"

	echo "PASS: distance smoke test completed successfully"
	echo "Script log file: ${LOG_FILE}"
	echo "Near phase log file: ${NEAR_LOG_FILE}"
	echo "Far phase log file: ${FAR_LOG_FILE}"
}

main "$@"