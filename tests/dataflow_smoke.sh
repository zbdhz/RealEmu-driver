#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REALWMEDIUMD_BIN="${REALWMEDIUMD_BIN:-${REPO_ROOT}/realwmediumd/realwmediumd}"
LOG_FILE="${LOG_FILE:-${SCRIPT_DIR}/dataflow_smoke.log}"
WORK_DIR="$(mktemp -d "${SCRIPT_DIR}/dataflow_smoke.XXXXXX")"
CFG_FILE="${WORK_DIR}/dataflow_smoke.cfg"

SUBNET="10.10.10"
NUM_PHYS=2
MESH_ID="realemu-smoke"
RADIO_MACS=(
	"02:00:00:00:00:00"
	"02:00:00:00:01:00"
)

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

	ip rule del priority 1000 >/dev/null 2>&1 || true
	ip rule add priority 0 table local >/dev/null 2>&1 || true
	echo 0 > /proc/sys/net/ipv4/conf/all/arp_ignore >/dev/null 2>&1 || true

	for i in $(seq 0 $((NUM_PHYS - 1))); do
		prio=$((i + 10))
		prio2=$((256 + prio))
		tbl="${prio2}"

		ip rule del priority "${prio2}" >/dev/null 2>&1 || true
		ip rule del priority "${prio}" >/dev/null 2>&1 || true
		ip route flush table "${tbl}" >/dev/null 2>&1 || true
	done

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	rm -rf "${WORK_DIR}"

	if [[ "${exit_code}" -ne 0 && -f "${LOG_FILE}" ]]; then
		echo "realwmediumd log tail:"
		tail -n 80 "${LOG_FILE}" || true
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
EOF
}

wait_for_hwsim_devices() {
	PHY_NAMES=()
	mapfile -t PHY_NAMES < <(ls -t /sys/class/ieee80211 2>/dev/null | head -n "${NUM_PHYS}")
	if [[ "${#PHY_NAMES[@]}" -ne "${NUM_PHYS}" ]]; then
		echo "Expected ${NUM_PHYS} hwsim phys, found ${#PHY_NAMES[@]}" >&2
		exit 1
	fi
}

configure_station() {
	local phy_name="$1"
	local mac_addr="$2"
	local ip_addr="$3"
	local prio="$4"

	local dev_name
	dev_name="$(ls /sys/class/ieee80211/${phy_name}/device/net)"
	DEVICES+=("${dev_name}")

	ip link set "${dev_name}" down
	ip link set address "${mac_addr}" dev "${dev_name}"
	iw dev "${dev_name}" set type mesh
	iw dev "${dev_name}" set channel 36
	ip link set "${dev_name}" up
	iw dev "${dev_name}" mesh join "${MESH_ID}"

	ip addr flush dev "${dev_name}"
	ip addr add "${ip_addr}/24" dev "${dev_name}"

	prio2=$((256 + prio))
	tbl="${prio2}"

	echo 1 > "/proc/sys/net/ipv4/conf/${dev_name}/accept_local"
	ip rule del priority "${prio}" >/dev/null 2>&1 || true
	ip rule add priority "${prio}" iif "${dev_name}" lookup local
	ip rule del priority "${prio2}" >/dev/null 2>&1 || true
	ip rule add priority "${prio2}" from "${ip_addr}" table "${tbl}"
	ip route flush table "${tbl}" >/dev/null 2>&1 || true
	ip route add default dev "${dev_name}" table "${tbl}"
}

main() {
	require_root
	require_cmd modprobe
	require_cmd ip
	require_cmd iw
	require_cmd ping
	require_cmd ls
	require_cmd tail

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

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	modprobe mac80211_hwsim radios="${NUM_PHYS}"

	wait_for_hwsim_devices

	configure_station "${PHY_NAMES[0]}" "${RADIO_MACS[0]}" "${SUBNET}.10" 10
	configure_station "${PHY_NAMES[1]}" "${RADIO_MACS[1]}" "${SUBNET}.11" 11

	"${REALWMEDIUMD_BIN}" -c "${CFG_FILE}" >"${LOG_FILE}" 2>&1 &
	REALWMEDIUMD_PID=$!

	if ! kill -0 "${REALWMEDIUMD_PID}" >/dev/null 2>&1; then
		echo "realwmediumd failed to start" >&2
		exit 1
	fi

	ping -I "${SUBNET}.10" -c 5 -W 1 "${SUBNET}.11"

	echo "PASS: data flow smoke test completed successfully"
	echo "Log file: ${LOG_FILE}"
}

main "$@"