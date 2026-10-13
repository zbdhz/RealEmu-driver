#!/usr/bin/env python3
"""UDP 吞吐测试: 无 TCP 控制连接, 纯 UDP 打流.
用法:
  server: python3 udp_blast.py server PORT DURATION
  client: python3 udp_blast.py client SERVER_IP PORT DURATION RATE_MBPS PKT_SIZE
输出 (server 端): 每秒实时输出 "sec N-N+1: X.XX Mbps"
"""
import socket, struct, time, sys, os, signal

def run_server(port, duration):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)  # 每秒唤醒一次, 实时输出
    start = time.time()
    cur_sec = 0
    cur_bytes = 0
    total = 0
    done = False
    while not done:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            pass
        else:
            if len(data) == 3 and data == b'END':
                done = True
                break
            now = time.time()
            sec = int(now - start)
            if sec > cur_sec:
                mbps = cur_bytes * 8 / 1e6
                print(f"sec {cur_sec}-{cur_sec+1}: {mbps:.2f} Mbps", flush=True)
                cur_sec = sec
                cur_bytes = 0
            cur_bytes += len(data)
            total += len(data)
        now = time.time()
        elapsed = int(now - start)
        if elapsed > cur_sec:
            mbps = cur_bytes * 8 / 1e6
            print(f"sec {cur_sec}-{cur_sec+1}: {mbps:.2f} Mbps", flush=True)
            cur_sec = elapsed
            cur_bytes = 0
        if elapsed >= duration + 5:
            break
    # 输出最后一秒
    if cur_bytes > 0:
        mbps = cur_bytes * 8 / 1e6
        print(f"sec {cur_sec}-{cur_sec+1}: {mbps:.2f} Mbps", flush=True)
    elapsed = time.time() - start
    print(f"total: {total*8/1e6/elapsed:.2f} Mbps avg over {elapsed:.1f}s", flush=True)

def run_client(server_ip, port, duration, rate_mbps, pkt_size):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024)
    interval_ns = pkt_size * 8 / (rate_mbps * 1e6)
    payload = os.urandom(pkt_size - 8)
    total_pkts = int(duration / interval_ns)
    start = time.time()
    next_ts = start
    for i in range(total_pkts):
        seq = struct.pack('>Q', i)
        sock.sendto(seq + payload, (server_ip, port))
        next_ts += interval_ns
        now = time.time()
        if next_ts > now:
            time.sleep(next_ts - now)
    for _ in range(20):
        sock.sendto(b'END', (server_ip, port))
        time.sleep(0.002)

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'server':
        run_server(int(sys.argv[2]), int(sys.argv[3]))
    elif mode == 'client':
        run_client(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
                   float(sys.argv[5]), int(sys.argv[6]))
    else:
        print(__doc__); sys.exit(1)
