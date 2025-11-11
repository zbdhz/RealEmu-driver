#!/usr/bin/env python3
import csv
import os

# 配置参数
NUM_NODES = 8          # 节点数量
TOTAL_SECONDS = 30     # 总秒数
OUTPUT_FILE = "channel_config.csv"  # 输出文件名

# 每秒的固定距离值（可以根据需要修改）
DISTANCE_VALUES = [
    1,  # 第1秒的距离值
    2,  # 第2秒的距离值
    3,  # 第3秒的距离值
    4,  # 第4秒的距离值
    5,  # 第5秒的距离值
    6,  # 第6秒的距离值
    40,  # 第7秒的距离值
    45,  # 第8秒的距离值
    50,  # 第9秒的距离值
    55,  # 第10秒的距离值
    60,  # 第11秒的距离值
    65,  # 第12秒的距离值
    70,  # 第13秒的距离值
    75,  # 第14秒的距离值
    80,  # 第15秒的距离值
    85,  # 第16秒的距离值
    90,  # 第17秒的距离值
    95,  # 第18秒的距离值
    100, # 第19秒的距离值
    105, # 第20秒的距离值
    110, # 第21秒的距离值
    115, # 第22秒的距离值
    120, # 第23秒的距离值
    125, # 第24秒的距离值
    130, # 第25秒的距离值
    135, # 第26秒的距离值
    140, # 第27秒的距离值
    145, # 第28秒的距离值
    150, # 第29秒的距离值
    155  # 第30秒的距离值
]

def generate_csv():
    """生成CSV文件"""
    # 确保有足够的距离值
    if len(DISTANCE_VALUES) < TOTAL_SECONDS:
        print(f"错误：距离值数量({len(DISTANCE_VALUES)})少于总秒数({TOTAL_SECONDS})")
        return False
    
    # 创建输出目录（如果不存在）
    output_dir = os.path.dirname(OUTPUT_FILE)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # 生成CSV文件
    with open(OUTPUT_FILE, 'w', newline='') as csvfile:
        writer = csv.writer(csvfile)
        
        # 写入标题行
        writer.writerow(['time_sec', 'src_id', 'dst_id', 'distance'])
        
        # 为每秒生成配置
        for second in range(1, TOTAL_SECONDS + 1):
            distance = DISTANCE_VALUES[second - 1]  # 获取当前秒的距离值
            
            # 为每对节点生成配置
            for src in range(NUM_NODES):
                for dst in range(NUM_NODES):
                    if src != dst:  # 跳过自己到自己的连接
                        writer.writerow([second, src, dst, distance])
    
    print(f"CSV文件已生成: {OUTPUT_FILE}")
    print(f"包含 {NUM_NODES} 个节点，{TOTAL_SECONDS} 秒的数据")
    print(f"每秒包含 {NUM_NODES * (NUM_NODES - 1)} 个配置")
    
    return True

def generate_c_define():
    """生成C语言define定义"""
    print("\n// C语言define定义（可复制到您的代码中）:")
    print(f"#define CSV_FILE_PATH \"{OUTPUT_FILE}\"")
    print(f"#define START_SECOND 1")
    print(f"#define END_SECOND {TOTAL_SECONDS}")
    print(f"#define MAX_CONFIGS_PER_SECOND {NUM_NODES * (NUM_NODES - 1)}")

if __name__ == "__main__":
    if generate_csv():
        generate_c_define()
