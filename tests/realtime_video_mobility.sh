#!/usr/bin/env bash

# Two-node RealEmu real-time video mobility scenario.
#
# The video stream is UDP unicast, so frames traverse the RealEmu/XDMA
# unicast path.  The current driver does not apply the software snr_matrix to
# unicast frames once they enter hardware, so the script also updates a
# distance-based netem profile.  This makes increasing distance observable as
# lower bandwidth, higher delay, and packet loss while still exercising FPGA.
#
# Typical use:
#   make -C realwmediumd
#   sudo bash tests/realtime_video_mobility.sh
#
# Useful overrides:
#   DURATION_SEC=90 SPEED_MPS=1.5 VIDEO_BITRATE_KBIT=1200 \
#     sudo -E bash tests/realtime_video_mobility.sh
#   VIDEO_SOURCE=/absolute/path/video.mp4 RECORD_FILE=/tmp/received.ts \
#     sudo -E bash tests/realtime_video_mobility.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REALWMEDIUMD_BIN="${REALWMEDIUMD_BIN:-${REPO_ROOT}/realwmediumd/realwmediumd}"

DURATION_SEC="${DURATION_SEC:-60}"
UPDATE_INTERVAL_SEC="${UPDATE_INTERVAL_SEC:-3}"
START_DISTANCE_M="${START_DISTANCE_M:-5}"
SPEED_MPS="${SPEED_MPS:-1.5}"
REFERENCE_SNR_DB="${REFERENCE_SNR_DB:-35}"
PATH_LOSS_EXPONENT="${PATH_LOSS_EXPONENT:-3.0}"

MIN_DELAY_MS="${MIN_DELAY_MS:-2}"
MAX_DELAY_MS="${MAX_DELAY_MS:-180}"
MIN_LOSS_PCT="${MIN_LOSS_PCT:-0}"
MAX_LOSS_PCT="${MAX_LOSS_PCT:-35}"
MIN_RATE_KBIT="${MIN_RATE_KBIT:-600}"
MAX_RATE_KBIT="${MAX_RATE_KBIT:-6000}"

VIDEO_PORT="${VIDEO_PORT:-5000}"
VIDEO_SIZE="${VIDEO_SIZE:-640x360}"
VIDEO_FPS="${VIDEO_FPS:-25}"
VIDEO_BITRATE_KBIT="${VIDEO_BITRATE_KBIT:-1500}"
VIDEO_CODEC="${VIDEO_CODEC:-libx264}"
VIDEO_SOURCE="${VIDEO_SOURCE:-}"
RECORD_FILE="${RECORD_FILE:-}"

SUBNET="10.10.20"
NUM_PHYS=2
TEST_SSID="realemu-video"
TEST_FREQ_MHZ=2412
TEST_BSSID="02:22:33:44:55:66"
WSERVER_SOCKET="/var/run/realwmediumd.sock"
RADIO_MACS=(
	"02:00:00:00:10:00"
	"02:00:00:00:11:00"
)
NS_NAMES=(
	"realemu-video-tx"
	"realemu-video-rx"
)

SCRIPT_LOG="${SCRIPT_LOG:-${SCRIPT_DIR}/realtime_video_mobility.log}"
WMEDIUMD_LOG_FILE="${WMEDIUMD_LOG_FILE:-${SCRIPT_DIR}/realwmediumd_video.log}"
SENDER_LOG_FILE="${SENDER_LOG_FILE:-${SCRIPT_DIR}/video_sender.log}"
RECEIVER_LOG_FILE="${RECEIVER_LOG_FILE:-${SCRIPT_DIR}/video_receiver.log}"
MOVEMENT_CSV="${MOVEMENT_CSV:-${SCRIPT_DIR}/video_mobility.csv}"

WORK_DIR="$(mktemp -d "${SCRIPT_DIR}/realtime_video.XXXXXX")"
CFG_FILE="${WORK_DIR}/realtime_video.cfg"
RECEIVER_PROGRESS="${WORK_DIR}/receiver.progress"

NS_PIDS=()
CREATED_NS_NAMES=()
PHY_NAMES=()
DEVICES=()
REALWMEDIUMD_PID=""
SENDER_PID=""
RECEIVER_PID=""
HWSIM_ACTIVE=0

log() {
	printf '[%(%F %T)T] %s\n' -1 "$*" | tee -a "${SCRIPT_LOG}"
}

cleanup() {
	local exit_code="$1"
	local pid

	set +e
	[[ -n "${SENDER_PID}" ]] && kill "${SENDER_PID}" >/dev/null 2>&1
	[[ -n "${RECEIVER_PID}" ]] && kill "${RECEIVER_PID}" >/dev/null 2>&1
	[[ -n "${SENDER_PID}" ]] && wait "${SENDER_PID}" >/dev/null 2>&1
	[[ -n "${RECEIVER_PID}" ]] && wait "${RECEIVER_PID}" >/dev/null 2>&1

	if [[ -n "${REALWMEDIUMD_PID}" ]]; then
		kill "${REALWMEDIUMD_PID}" >/dev/null 2>&1
		wait "${REALWMEDIUMD_PID}" >/dev/null 2>&1
	fi

	for pid in "${NS_PIDS[@]:-}"; do
		[[ -n "${pid}" ]] && kill "${pid}" >/dev/null 2>&1
	done
	for ns in "${CREATED_NS_NAMES[@]:-}"; do
		[[ -n "${ns}" ]] || continue
		for pid in $(ip netns pids "${ns}" 2>/dev/null); do
			kill "${pid}" >/dev/null 2>&1
		done
		ip netns del "${ns}" >/dev/null 2>&1
	done

	if [[ "${HWSIM_ACTIVE}" -eq 1 ]]; then
		modprobe -r mac80211_hwsim >/dev/null 2>&1
	fi
	rm -rf -- "${WORK_DIR}"

	if [[ "${exit_code}" -ne 0 ]]; then
		echo "Scenario failed (exit=${exit_code}). Log tails:" >&2
		for file in "${WMEDIUMD_LOG_FILE}" "${SENDER_LOG_FILE}" "${RECEIVER_LOG_FILE}"; do
			if [[ -f "${file}" ]]; then
				echo "=== ${file} ===" >&2
				tail -n 40 "${file}" >&2 || true
			fi
		done
	fi
}

trap 'exit 130' INT
trap 'exit 143' TERM
trap 'cleanup "$?"' EXIT

die() {
	echo "ERROR: $*" >&2
	exit 1
}

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

require_root() {
	[[ "${EUID}" -eq 0 ]] || die "Please run this scenario as root."
}

validate_positive_integer() {
	local name="$1"
	local value="$2"
	[[ "${value}" =~ ^[1-9][0-9]*$ ]] || die "${name} must be a positive integer (got: ${value})"
}

validate_nonnegative_number() {
	local name="$1"
	local value="$2"
	awk -v value="${value}" 'BEGIN { exit !(value ~ /^[0-9]+([.][0-9]+)?$/) }' || \
		die "${name} must be a non-negative number (got: ${value})"
}

validate_number() {
	local name="$1"
	local value="$2"
	awk -v value="${value}" 'BEGIN { exit !(value ~ /^-?[0-9]+([.][0-9]+)?$/) }' || \
		die "${name} must be a number (got: ${value})"
}

preflight() {
	require_root
	for cmd in modprobe ip iw ping tc ffmpeg python3 awk grep sed tail tee mktemp; do
		require_cmd "${cmd}"
	done

	validate_positive_integer DURATION_SEC "${DURATION_SEC}"
	validate_positive_integer UPDATE_INTERVAL_SEC "${UPDATE_INTERVAL_SEC}"
	validate_positive_integer VIDEO_PORT "${VIDEO_PORT}"
	validate_positive_integer VIDEO_FPS "${VIDEO_FPS}"
	validate_positive_integer VIDEO_BITRATE_KBIT "${VIDEO_BITRATE_KBIT}"
	validate_positive_integer MIN_RATE_KBIT "${MIN_RATE_KBIT}"
	validate_positive_integer MAX_RATE_KBIT "${MAX_RATE_KBIT}"
	validate_number REFERENCE_SNR_DB "${REFERENCE_SNR_DB}"
	validate_nonnegative_number START_DISTANCE_M "${START_DISTANCE_M}"
	validate_nonnegative_number SPEED_MPS "${SPEED_MPS}"
	validate_nonnegative_number PATH_LOSS_EXPONENT "${PATH_LOSS_EXPONENT}"
	validate_nonnegative_number MIN_DELAY_MS "${MIN_DELAY_MS}"
	validate_nonnegative_number MAX_DELAY_MS "${MAX_DELAY_MS}"
	validate_nonnegative_number MIN_LOSS_PCT "${MIN_LOSS_PCT}"
	validate_nonnegative_number MAX_LOSS_PCT "${MAX_LOSS_PCT}"
	awk -v distance="${START_DISTANCE_M}" 'BEGIN { exit !(distance > 0) }' || \
		die "START_DISTANCE_M must be greater than zero"
	awk \
		-v port="${VIDEO_PORT}" \
		-v min_delay="${MIN_DELAY_MS}" -v max_delay="${MAX_DELAY_MS}" \
		-v min_loss="${MIN_LOSS_PCT}" -v max_loss="${MAX_LOSS_PCT}" \
		-v min_rate="${MIN_RATE_KBIT}" -v max_rate="${MAX_RATE_KBIT}" '
		BEGIN {
			exit !(port <= 65535 && min_delay <= max_delay &&
			       min_loss <= max_loss && max_loss <= 100 &&
			       min_rate <= max_rate)
		}' || die "Invalid port or min/max link-profile values"

	[[ -x "${REALWMEDIUMD_BIN}" ]] || \
		die "realwmediumd binary not found: ${REALWMEDIUMD_BIN} (run: make -C realwmediumd)"
	ffmpeg -hide_banner -encoders 2>/dev/null | \
		awk -v codec="${VIDEO_CODEC}" '$2 == codec { found = 1 } END { exit !found }' || \
		die "ffmpeg encoder is unavailable: ${VIDEO_CODEC}"

	if [[ -n "${VIDEO_SOURCE}" ]]; then
		[[ "${VIDEO_SOURCE}" = /* ]] || die "VIDEO_SOURCE must be an absolute path"
		[[ -r "${VIDEO_SOURCE}" ]] || die "VIDEO_SOURCE is not readable: ${VIDEO_SOURCE}"
	fi
	if [[ -n "${RECORD_FILE}" ]]; then
		[[ "${RECORD_FILE}" = /* ]] || die "RECORD_FILE must be an absolute path"
		[[ -d "$(dirname "${RECORD_FILE}")" ]] || \
			die "RECORD_FILE parent directory does not exist: $(dirname "${RECORD_FILE}")"
	fi

	for device_path in /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user; do
		[[ -e "${device_path}" ]] || die "Missing XDMA device: ${device_path}"
	done
}

prepare_config() {
	cat >"${CFG_FILE}" <<EOF
ifaces :
{
	count = 2;
	ids = ["${RADIO_MACS[0]}", "${RADIO_MACS[1]}" ];
};

model :
{
	type = "snr";
	links = ( );
	default_snr = ${REFERENCE_SNR_DB};
};
EOF
}

wait_for_hwsim_devices() {
	local phy phy_name dev_path
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
	[[ "${#PHY_NAMES[@]}" -eq "${NUM_PHYS}" ]] || \
		die "Expected ${NUM_PHYS} mac80211_hwsim phys, found ${#PHY_NAMES[@]}"
}

create_namespaces() {
	local ns
	for ns in "${NS_NAMES[@]}"; do
		[[ ! -e "/var/run/netns/${ns}" ]] || \
			die "Network namespace already exists: ${ns}"
		ip netns add "${ns}"
		CREATED_NS_NAMES+=("${ns}")
		ip -n "${ns}" link set lo up
		ip netns exec "${ns}" sleep 86400 &
		NS_PIDS+=("$!")
	done
}

configure_station() {
	local phy_name="$1"
	local ns_name="$2"
	local mac_addr="$3"
	local ip_addr="$4"
	local dev_name

	dev_name="$(ls "/sys/class/ieee80211/${phy_name}/device/net" | head -n 1)"
	log "Configuring ${dev_name} (${phy_name}) in ${ns_name}"
	ip link set "${dev_name}" down
	ip link set address "${mac_addr}" dev "${dev_name}"
	iw phy "${phy_name}" set netns name "${ns_name}"

	dev_name="$(ip netns exec "${ns_name}" ls "/sys/class/ieee80211/${phy_name}/device/net" | head -n 1)"
	DEVICES+=("${dev_name}")
	ip -n "${ns_name}" link set "${dev_name}" down || true
	ip netns exec "${ns_name}" iw dev "${dev_name}" set type ibss
	ip -n "${ns_name}" link set "${dev_name}" up
	ip netns exec "${ns_name}" iw dev "${dev_name}" ibss join \
		"${TEST_SSID}" "${TEST_FREQ_MHZ}" fixed-freq "${TEST_BSSID}"
	ip -n "${ns_name}" addr flush dev "${dev_name}"
	ip -n "${ns_name}" addr add "${ip_addr}/24" dev "${dev_name}"
	ip -n "${ns_name}" route replace "${SUBNET}.0/24" dev "${dev_name}"
}

wait_for_socket() {
	local attempt
	for ((attempt = 0; attempt < 50; attempt++)); do
		[[ -S "${WSERVER_SOCKET}" ]] && return 0
		kill -0 "${REALWMEDIUMD_PID}" >/dev/null 2>&1 || return 1
		sleep 0.1
	done
	return 1
}

# Send the packed wserver SNR request (type=1) and validate its response.
update_snr() {
	local snr="$1"
	python3 - "${WSERVER_SOCKET}" "${RADIO_MACS[0]}" "${RADIO_MACS[1]}" "${snr}" <<'PY'
import socket
import struct
import sys

socket_path, from_mac, to_mac, snr_text = sys.argv[1:]

def mac_bytes(value):
    return bytes(int(part, 16) for part in value.split(':'))

def recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("wserver closed before a complete response")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b''.join(chunks)

request = struct.pack(
    "!B6s6si", 1, mac_bytes(from_mac), mac_bytes(to_mac), int(snr_text)
)
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.settimeout(2.0)
    client.connect(socket_path)
    client.sendall(request)
    response = recv_exact(client, 19)

if response[0] != 2:
    raise RuntimeError(f"unexpected wserver response type: {response[0]}")
if response[-1] != 0:
    raise RuntimeError(f"wserver rejected SNR update: result={response[-1]}")
PY
}

calculate_profile() {
	local elapsed="$1"
	awk \
		-v elapsed="${elapsed}" \
		-v duration="${DURATION_SEC}" \
		-v start="${START_DISTANCE_M}" \
		-v speed="${SPEED_MPS}" \
		-v ref_snr="${REFERENCE_SNR_DB}" \
		-v exponent="${PATH_LOSS_EXPONENT}" \
		-v min_delay="${MIN_DELAY_MS}" \
		-v max_delay="${MAX_DELAY_MS}" \
		-v min_loss="${MIN_LOSS_PCT}" \
		-v max_loss="${MAX_LOSS_PCT}" \
		-v min_rate="${MIN_RATE_KBIT}" \
		-v max_rate="${MAX_RATE_KBIT}" '
	BEGIN {
		distance = start + speed * elapsed
		progress = elapsed / duration
		if (progress < 0) progress = 0
		if (progress > 1) progress = 1
		# A quadratic loss curve preserves video quality near the start and
		# makes degradation clearly visible near the end of the run.
		delay = min_delay + (max_delay - min_delay) * progress
		loss = min_loss + (max_loss - min_loss) * progress * progress
		rate = max_rate - (max_rate - min_rate) * progress
		snr = ref_snr - 10 * exponent * log(distance / start) / log(10)
		printf "%.2f,%d,%.2f,%d,%d\n", distance, snr, loss, delay, rate
	}'
}

apply_link_profile() {
	local delay_ms="$1"
	local loss_pct="$2"
	local rate_kbit="$3"
	local index

	for index in 0 1; do
		ip netns exec "${NS_NAMES[${index}]}" tc qdisc replace \
			dev "${DEVICES[${index}]}" root netem \
			delay "${delay_ms}ms" loss "${loss_pct}%" rate "${rate_kbit}kbit"
	done
}

start_receiver() {
	local -a output_args
	if [[ -n "${RECORD_FILE}" ]]; then
		output_args=(-map 0:v:0 -c copy -y "${RECORD_FILE}")
	else
		output_args=(-map 0:v:0 -f null -)
	fi

	ip netns exec "${NS_NAMES[1]}" ffmpeg \
		-hide_banner -loglevel info -nostats \
		-fflags nobuffer \
		-i "udp://0.0.0.0:${VIDEO_PORT}?fifo_size=1000000&overrun_nonfatal=1" \
		-progress "${RECEIVER_PROGRESS}" \
		"${output_args[@]}" >"${RECEIVER_LOG_FILE}" 2>&1 &
	RECEIVER_PID=$!
}

start_sender() {
	local -a input_args
	local -a codec_args
	if [[ -n "${VIDEO_SOURCE}" ]]; then
		input_args=(-re -stream_loop -1 -i "${VIDEO_SOURCE}")
	else
		input_args=(-re -f lavfi -i "testsrc2=size=${VIDEO_SIZE}:rate=${VIDEO_FPS}")
	fi
	if [[ "${VIDEO_CODEC}" == "libx264" ]]; then
		codec_args=(-preset ultrafast -tune zerolatency)
	else
		codec_args=()
	fi

	ip netns exec "${NS_NAMES[0]}" ffmpeg \
		-hide_banner -loglevel info -nostats \
		"${input_args[@]}" \
		-t "${DURATION_SEC}" -an \
		-c:v "${VIDEO_CODEC}" -pix_fmt yuv420p \
		"${codec_args[@]}" \
		-g "${VIDEO_FPS}" \
		-b:v "${VIDEO_BITRATE_KBIT}k" \
		-maxrate "${VIDEO_BITRATE_KBIT}k" \
		-bufsize "$((VIDEO_BITRATE_KBIT * 2))k" \
		-f mpegts "udp://${SUBNET}.11:${VIDEO_PORT}?pkt_size=1316" \
		>"${SENDER_LOG_FILE}" 2>&1 &
	SENDER_PID=$!
}

run_mobility_loop() {
	local sim_start elapsed profile distance snr loss delay rate
	local ping_status rtt ping_output sleep_for

	printf 'elapsed_s,distance_m,snr_db,delay_ms,loss_pct,rate_kbit,ping_ok,rtt_ms\n' >"${MOVEMENT_CSV}"
	sim_start="${SECONDS}"

	while true; do
		elapsed=$((SECONDS - sim_start))
		(( elapsed <= DURATION_SEC )) || break
		kill -0 "${SENDER_PID}" >/dev/null 2>&1 || break

		profile="$(calculate_profile "${elapsed}")"
		IFS=',' read -r distance snr loss delay rate <<<"${profile}"
		apply_link_profile "${delay}" "${loss}" "${rate}"
		if ! update_snr "${snr}"; then
			log "WARNING: wserver rejected SNR=${snr} update; netem profile remains active"
		fi

		ping_status=0
		rtt=""
		if ping_output="$(LC_ALL=C ip netns exec "${NS_NAMES[0]}" \
			ping -c 1 -W 1 "${SUBNET}.11" 2>/dev/null)"; then
			ping_status=1
			rtt="$(sed -n 's/.*time=\([^ ]*\).*/\1/p' <<<"${ping_output}" | tail -n 1)"
		fi
		printf '%d,%s,%s,%s,%s,%s,%d,%s\n' \
			"${elapsed}" "${distance}" "${snr}" "${delay}" "${loss}" "${rate}" \
			"${ping_status}" "${rtt}" >>"${MOVEMENT_CSV}"
		log "t=${elapsed}s distance=${distance}m SNR=${snr}dB delay=${delay}ms loss=${loss}% rate=${rate}kbit ping=${ping_status}"

		sleep_for="${UPDATE_INTERVAL_SEC}"
		if (( elapsed + sleep_for > DURATION_SEC )); then
			sleep_for=$((DURATION_SEC - elapsed))
		fi
		(( sleep_for > 0 )) || break
		sleep "${sleep_for}"
	done
}

count_received_frames() {
	local frames
	frames="$(sed -n 's/^frame=//p' "${RECEIVER_PROGRESS}" 2>/dev/null | tail -n 1)"
	frames="${frames//[[:space:]]/}"
	if [[ "${frames}" =~ ^[0-9]+$ ]]; then
		echo "${frames}"
	else
		echo 0
	fi
}

assert_hardware_activity() {
	local tx_count hw_complete
	tx_count="$(grep -c '\[TX\] Frame queued to TX queue: 0->1' "${WMEDIUMD_LOG_FILE}" || true)"
	hw_complete="$(grep -c '\[COMPLETE\] Frame cookie=.*ACKed by hardware' "${WMEDIUMD_LOG_FILE}" || true)"
	log "RealEmu counters: TX 0->1=${tx_count}, hardware completions=${hw_complete}"
	[[ "${tx_count}" -gt 0 && "${hw_complete}" -gt 0 ]] || \
		die "No confirmed RealEmu hardware video data-plane activity"
}

main() {
	local initial_profile distance snr loss delay rate
	local sender_exit receiver_frames

	preflight
	: >"${SCRIPT_LOG}"
	: >"${WMEDIUMD_LOG_FILE}"
	: >"${SENDER_LOG_FILE}"
	: >"${RECEIVER_LOG_FILE}"
	prepare_config

	log "Starting ${DURATION_SEC}s scenario at ${START_DISTANCE_M}m, separation speed=${SPEED_MPS}m/s"
	log "Video: ${VIDEO_SIZE}@${VIDEO_FPS}fps, codec=${VIDEO_CODEC}, target=${VIDEO_BITRATE_KBIT}kbit/s"

	modprobe -r mac80211_hwsim >/dev/null 2>&1 || true
	sleep 1
	modprobe mac80211_hwsim radios="${NUM_PHYS}"
	HWSIM_ACTIVE=1
	sleep 1
	wait_for_hwsim_devices
	create_namespaces
	configure_station "${PHY_NAMES[0]}" "${NS_NAMES[0]}" "${RADIO_MACS[0]}" "${SUBNET}.10"
	configure_station "${PHY_NAMES[1]}" "${NS_NAMES[1]}" "${RADIO_MACS[1]}" "${SUBNET}.11"

	"${REALWMEDIUMD_BIN}" -l 7 -s -c "${CFG_FILE}" >>"${WMEDIUMD_LOG_FILE}" 2>&1 &
	REALWMEDIUMD_PID=$!
	wait_for_socket || die "realwmediumd or its wserver failed to start"
	log "realwmediumd started (PID=${REALWMEDIUMD_PID})"

	initial_profile="$(calculate_profile 0)"
	IFS=',' read -r distance snr loss delay rate <<<"${initial_profile}"
	apply_link_profile "${delay}" "${loss}" "${rate}"
	update_snr "${snr}"
	ip netns exec "${NS_NAMES[0]}" ping -c 3 -W 1 "${SUBNET}.11" >>"${SCRIPT_LOG}" 2>&1 || \
		die "Initial connectivity check failed"

	start_receiver
	sleep 1
	kill -0 "${RECEIVER_PID}" >/dev/null 2>&1 || die "Video receiver exited during startup"
	start_sender
	sleep 1
	kill -0 "${SENDER_PID}" >/dev/null 2>&1 || die "Video sender exited during startup"
	log "UDP real-time video started: ${SUBNET}.10 -> ${SUBNET}.11:${VIDEO_PORT}"

	run_mobility_loop

	set +e
	wait "${SENDER_PID}"
	sender_exit=$?
	set -e
	SENDER_PID=""
	sleep 2
	if [[ -n "${RECEIVER_PID}" ]]; then
		kill "${RECEIVER_PID}" >/dev/null 2>&1 || true
		wait "${RECEIVER_PID}" >/dev/null 2>&1 || true
		RECEIVER_PID=""
	fi
	[[ "${sender_exit}" -eq 0 ]] || die "ffmpeg sender failed with exit code ${sender_exit}"

	receiver_frames="$(count_received_frames)"
	log "Receiver decoded/processed frames: ${receiver_frames}"
	[[ "${receiver_frames}" -gt 0 ]] || die "Receiver did not process any video frames"
	assert_hardware_activity

	log "PASS: moving-node real-time video scenario completed"
	log "Movement metrics: ${MOVEMENT_CSV}"
	log "Sender log: ${SENDER_LOG_FILE}"
	log "Receiver log: ${RECEIVER_LOG_FILE}"
	[[ -z "${RECORD_FILE}" ]] || log "Received stream recording: ${RECORD_FILE}"
}

main "$@"
