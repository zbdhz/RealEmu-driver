#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
from collections import defaultdict
import math
import matplotlib.pyplot as plt
import numpy as np

def parse_log_file(log_file):
    """解析日志文件，返回按mpducacheaddr分组的事件"""
    events = defaultdict(list)
    
    if not os.path.exists(log_file):
        print(f"警告: 文件 {log_file} 不存在")
        return events
    
    with open(log_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            # 解析日志行: [timestamp] ACTION seq=X mpducacheaddr=0xY src=A dst=B
            match = re.match(r'\[(\d+)\]\s+(\w+)\s+seq=(\d+)\s+mpducacheaddr=0x([0-9A-Fa-f]+)\s+src=(\d+)\s+dst=(\d+)', line)
            if match:
                timestamp = int(match.group(1))
                action = match.group(2)
                seq = int(match.group(3))
                mpducacheaddr = match.group(4).upper()
                src = int(match.group(5))
                dst = int(match.group(6))
                
                events[mpducacheaddr].append({
                    'timestamp': timestamp,
                    'action': action,
                    'seq': seq,
                    'src': src,
                    'dst': dst
                })
    
    return events

def calculate_hop_delays(node_events):
    """计算每一跳的时延"""
    hop_delays = defaultdict(list)
    
    # 遍历所有数据包
    for mpducacheaddr in node_events[0]:
        # 获取节点0的发送事件
        node0_events = [e for e in node_events[0][mpducacheaddr] if e['action'] == 'SEND']
        if not node0_events:
            continue
        
        send_time = node0_events[0]['timestamp']
        
        # 计算从节点0到节点1的时延
        if 1 in node_events and mpducacheaddr in node_events[1]:
            node1_events = [e for e in node_events[1][mpducacheaddr] if e['action'] == 'RECEIVE']
            if node1_events:
                receive_time = node1_events[0]['timestamp']
                delay = receive_time - send_time
                hop_delays['0->1'].append(delay)
        
        # 计算从节点1到节点2的时延
        if 1 in node_events and 2 in node_events:
            node1_forward = [e for e in node_events[1].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node2_receive = [e for e in node_events[2].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node1_forward and node2_receive:
                forward_time = node1_forward[0]['timestamp']
                receive_time = node2_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['1->2'].append(delay)
        
        # 计算从节点2到节点3的时延
        if 2 in node_events and 3 in node_events:
            node2_forward = [e for e in node_events[2].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node3_receive = [e for e in node_events[3].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node2_forward and node3_receive:
                forward_time = node2_forward[0]['timestamp']
                receive_time = node3_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['2->3'].append(delay)
        
        # 计算从节点3到节点4的时延
        if 3 in node_events and 4 in node_events:
            node3_forward = [e for e in node_events[3].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node4_receive = [e for e in node_events[4].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node3_forward and node4_receive:
                forward_time = node3_forward[0]['timestamp']
                receive_time = node4_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['3->4'].append(delay)
        
        # 计算从节点4到节点5的时延
        if 4 in node_events and 5 in node_events:
            node4_forward = [e for e in node_events[4].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node5_receive = [e for e in node_events[5].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node4_forward and node5_receive:
                forward_time = node4_forward[0]['timestamp']
                receive_time = node5_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['4->5'].append(delay)
        
        # 计算从节点5到节点6的时延
        if 5 in node_events and 6 in node_events:
            node5_forward = [e for e in node_events[5].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node6_receive = [e for e in node_events[6].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node5_forward and node6_receive:
                forward_time = node5_forward[0]['timestamp']
                receive_time = node6_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['5->6'].append(delay)
        
        # 计算从节点6到节点7的时延
        if 6 in node_events and 7 in node_events:
            node6_forward = [e for e in node_events[6].get(mpducacheaddr, []) if e['action'] == 'FORWARD']
            node7_receive = [e for e in node_events[7].get(mpducacheaddr, []) if e['action'] == 'RECEIVE']
            
            if node6_forward and node7_receive:
                forward_time = node6_forward[0]['timestamp']
                receive_time = node7_receive[0]['timestamp']
                delay = receive_time - forward_time
                hop_delays['6->7'].append(delay)
    
    return hop_delays

def calculate_stats(delays):
    """计算时延的统计信息"""
    if not delays:
        return None
    
    n = len(delays)
    mean = sum(delays) / n
    
    if n > 1:
        variance = sum((x - mean) ** 2 for x in delays) / (n - 1)
        std_dev = math.sqrt(variance)
    else:
        std_dev = 0
    
    min_delay = min(delays)
    max_delay = max(delays)
    
    return {
        'count': n,
        'mean': mean,
        'std_dev': std_dev,
        'min': min_delay,
        'max': max_delay
    }

def plot_end_to_end_delay(node_events):
    """绘制端到端时延箱线图和折线图"""
    # 计算从节点0到每个节点的端到端时延
    hop_delays = {}  # 存储每个节点的时延数据
    hop_stats = {}   # 存储统计信息
    
    for target_node in range(1, 8):
        end_to_end_delays = []
        for mpducacheaddr in node_events[0]:
            node0_send = [e for e in node_events[0][mpducacheaddr] if e['action'] == 'SEND']
            if not node0_send:
                continue
            
            send_time = node0_send[0]['timestamp']
            
            if target_node in node_events and mpducacheaddr in node_events[target_node]:
                target_receive = [e for e in node_events[target_node][mpducacheaddr] if e['action'] == 'RECEIVE']
                if target_receive:
                    receive_time = target_receive[0]['timestamp']
                    delay = receive_time - send_time
                    end_to_end_delays.append(delay)
        
        if end_to_end_delays:
            hop_delays[target_node] = [d / 1000 for d in end_to_end_delays]  # 转换为毫秒
            hop_stats[target_node] = calculate_stats(end_to_end_delays)
    
    # 设置中文字体
    plt.rcParams['font.sans-serif'] = ['DejaVu Sans', 'Arial', 'sans-serif']
    plt.rcParams['axes.unicode_minus'] = False
    
    # 创建图表
    fig, ax = plt.subplots(figsize=(10, 6))
    
    # 准备箱线图数据
    data = [hop_delays[i] for i in sorted(hop_delays.keys())]
    positions = list(sorted(hop_delays.keys()))
    
    # 绘制散点图（显示数据分布，只采样部分数据避免过于密集）
    for i, pos in enumerate(positions):
        delays = data[i]
        # 只采样50个点，避免过于密集
        if len(delays) > 200:
            sampled_indices = np.random.choice(len(delays), 200, replace=False)
            sampled_delays = [delays[j] for j in sampled_indices]
        else:
            sampled_delays = delays
        # 添加抖动避免重叠
        jitter = np.random.normal(0, 0.06, len(sampled_delays))
        x_positions = [pos + j for j in jitter]
        ax.scatter(x_positions, sampled_delays, alpha=0.5, s=15, color='#5B9BD5', 
                  edgecolors='none', zorder=3)
    
    # 绘制箱线图（统一蓝色系，与折线协调）
    bp = ax.boxplot(data, positions=positions, widths=0.5, 
                    patch_artist=True, showfliers=False)
    
    # 设置箱线图统一颜色（与折线协调的蓝色系）
    for patch in bp['boxes']:
        patch.set_facecolor('#B4C6E7')  # 浅蓝灰色
        patch.set_alpha(0.6)
        patch.set_edgecolor('#2F5597')  # 深蓝色边框
        patch.set_linewidth(1.5)
    
    # 设置线条颜色（与折线协调）
    for whisker in bp['whiskers']:
        whisker.set(color='#2F5597', linewidth=1.5, linestyle='-')
    
    for cap in bp['caps']:
        cap.set(color='#2F5597', linewidth=2)
    
    for median in bp['medians']:
        median.set(color='#1F3864', linewidth=2)  # 深海军蓝
    
    # 绘制折线图（平均值，降低饱和度）
    means = [hop_stats[i]['mean'] / 1000 for i in positions]
    ax.plot(positions, means, 'o-', color='#2F5597', linewidth=2.5, 
            markersize=10, markerfacecolor='#5B9BD5', markeredgecolor='#2F5597',
            markeredgewidth=2, label='Mean Delay', zorder=5)
    
    # 设置图表标题和标签
    ax.set_title('End-to-End Delay Distribution vs Hop Count', fontsize=14, fontweight='bold')
    ax.set_xlabel('Hop Count (Target Node)', fontsize=12)
    ax.set_ylabel('Delay [ms]', fontsize=12)
    
    # 设置x轴刻度
    ax.set_xticks(positions)
    ax.set_xticklabels([f'Node {i}' for i in positions])
    
    # 添加图例
    ax.legend(loc='upper left', fontsize=10)
    
    # 添加网格
    ax.yaxis.grid(True, linestyle='--', alpha=0.7)
    ax.set_axisbelow(True)
    
    # 调整布局
    plt.tight_layout()
    
    # 保存图表为PNG
    plt.savefig('/home/gtx/dev/RealEmu-driver-20260312/realemu-hw/end_to_end_delay_combined.png', 
                dpi=300, bbox_inches='tight')
    print("\n组合图已保存到: end_to_end_delay_combined.png")
    
    # 保存图表为PDF
    plt.savefig('/home/gtx/dev/RealEmu-driver-20260312/realemu-hw/end_to_end_delay_combined.pdf', 
                format='pdf', bbox_inches='tight')
    print("PDF已保存到: end_to_end_delay_combined.pdf")
    
    # 显示图表
    plt.show()

def main():
    log_dir = '/home/gtx/dev/RealEmu-driver-20260312/realemu-hw'
    node_events = {}
    
    # 解析所有节点的日志文件
    for i in range(8):
        log_file = os.path.join(log_dir, f'node_{i}.log')
        node_events[i] = parse_log_file(log_file)
        print(f"节点 {i}: 解析了 {len(node_events[i])} 个数据包")
    
    print()
    
    # 计算每一跳的时延
    hop_delays = calculate_hop_delays(node_events)
    
    # 输出结果
    print("=" * 80)
    print("多跳网络时延分析")
    print("=" * 80)
    print()
    
    hops = ['0->1', '1->2', '2->3', '3->4', '4->5', '5->6', '6->7']
    
    for hop in hops:
        if hop in hop_delays and hop_delays[hop]:
            stats = calculate_stats(hop_delays[hop])
            print(f"跳 {hop}:")
            print(f"  数据包数量: {stats['count']}")
            print(f"  平均时延: {stats['mean']:.2f} 微秒 ({stats['mean']/1000:.2f} 毫秒)")
            print(f"  标准差: {stats['std_dev']:.2f} 微秒 ({stats['std_dev']/1000:.2f} 毫秒)")
            print(f"  最小时延: {stats['min']:.2f} 微秒 ({stats['min']/1000:.2f} 毫秒)")
            print(f"  最大时延: {stats['max']:.2f} 微秒 ({stats['max']/1000:.2f} 毫秒)")
            print()
        else:
            print(f"跳 {hop}: 无数据")
            print()
    
    # 计算端到端时延（从节点0发送到每个节点接收）
    print("=" * 80)
    print("端到端时延统计（节点0 -> 各节点）")
    print("=" * 80)
    print()
    
    for target_node in range(1, 8):
        end_to_end_delays = []
        for mpducacheaddr in node_events[0]:
            node0_send = [e for e in node_events[0][mpducacheaddr] if e['action'] == 'SEND']
            if not node0_send:
                continue
            
            send_time = node0_send[0]['timestamp']
            
            if target_node in node_events and mpducacheaddr in node_events[target_node]:
                target_receive = [e for e in node_events[target_node][mpducacheaddr] if e['action'] == 'RECEIVE']
                if target_receive:
                    receive_time = target_receive[0]['timestamp']
                    delay = receive_time - send_time
                    end_to_end_delays.append(delay)
        
        if end_to_end_delays:
            stats = calculate_stats(end_to_end_delays)
            print(f"节点0 -> 节点{target_node}:")
            print(f"  数据包数量: {stats['count']}")
            print(f"  平均时延: {stats['mean']:.2f} 微秒 ({stats['mean']/1000:.2f} 毫秒)")
            print(f"  标准差: {stats['std_dev']:.2f} 微秒 ({stats['std_dev']/1000:.2f} 毫秒)")
            print(f"  最小时延: {stats['min']:.2f} 微秒 ({stats['min']/1000:.2f} 毫秒)")
            print(f"  最大时延: {stats['max']:.2f} 微秒 ({stats['max']/1000:.2f} 毫秒)")
            print()
        else:
            print(f"节点0 -> 节点{target_node}: 无数据")
            print()
    
    # 绘制端到端时延图表
    plot_end_to_end_delay(node_events)

if __name__ == '__main__':
    main()
