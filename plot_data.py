#!/usr/bin/env python3
"""
使用matplotlib生成CPU压力测试结果的图表
从CSV文件读取数据并生成PNG图表
不依赖pandas，只使用标准库和matplotlib
"""

import matplotlib
matplotlib.use('Agg')  # 使用Agg后端（无显示器）
import matplotlib.pyplot as plt
import csv
import sys
import os

def read_csv_data(csv_file):
    """
    读取CSV文件并返回数据字典
    
    Args:
        csv_file: CSV文件路径
    
    Returns:
        包含列数据的字典，键为列名，值为数据列表
    """
    data = {}
    
    try:
        with open(csv_file, 'r') as f:
            reader = csv.reader(f)
            headers = next(reader)  # 读取头行
            
            # 初始化所有列
            for header in headers:
                data[header] = []
            
            # 读取数据行
            for row in reader:
                for i, header in enumerate(headers):
                    try:
                        # 尝试转换为浮点数
                        value = float(row[i])
                        data[header].append(value)
                    except (ValueError, IndexError):
                        # 如果转换失败，保存原始值
                        data[header].append(row[i])
        
        return data
    except Exception as e:
        print(f"Error reading CSV file: {e}", file=sys.stderr)
        return None

def plot_csv_data(csv_file, output_file="cpu_stress_result.png"):
    """
    读取CSV文件并生成图表
    
    Args:
        csv_file: CSV文件路径
        output_file: 输出PNG文件路径
    """
    try:
        # 读取CSV文件
        data = read_csv_data(csv_file)
        if data is None:
            return False
        
        print(f"Successfully loaded {csv_file}")
        print(f"Total columns: {len(data)}")
        
        # 提取时间列
        time = data.get('Time(s)', [])
        if not time:
            print("Error: No Time(s) column found in CSV", file=sys.stderr)
            return False
        
        print(f"Data points: {len(time)}")
        
        # 检测CPU核心数
        core_usage_columns = [col for col in data.keys() if 'Core' in col and 'Usage(%)' in col]
        num_cores = len(core_usage_columns)
        print(f"Detected {num_cores} CPU cores")
        
        # 创建图表，4个子图
        fig, axes = plt.subplots(4, 1, figsize=(12, 12))
        fig.suptitle('CPU Stress Test Results', fontsize=16, fontweight='bold')
        
        # 子图1: CPU使用率（限制16个核心以保持可读性）
        ax1 = axes[0]
        for i in range(min(num_cores, 16)):
            col = f'Core{i}_Usage(%)'
            if col in data:
                ax1.plot(time, data[col], label=f'Core {i}', alpha=0.7)
        ax1.set_title('CPU Core Usage History')
        ax1.set_ylabel('Usage (%)')
        ax1.set_ylim(-5, 105)
        ax1.grid(True, alpha=0.3)
        ax1.legend(loc='best', fontsize=8)
        if num_cores > 16:
            ax1.text(0.02, 0.95, f'Showing first 16 of {num_cores} cores', 
                    transform=ax1.transAxes, fontsize=9, verticalalignment='top',
                    bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
        
        # 子图2: CPU频率
        ax2 = axes[1]
        freq_columns = [col for col in data.keys() if 'Freq(MHz)' in col]
        for i in range(min(len(freq_columns), 16)):
            col = f'Core{i}_Freq(MHz)'
            if col in data:
                ax2.plot(time, data[col], label=f'Core {i}', alpha=0.7)
        ax2.set_title('CPU Core Frequency History')
        ax2.set_ylabel('Frequency (MHz)')
        ax2.grid(True, alpha=0.3)
        if len(freq_columns) > 16:
            ax2.text(0.02, 0.95, f'Showing first 16 of {len(freq_columns)} cores', 
                    transform=ax2.transAxes, fontsize=9, verticalalignment='top',
                    bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
        
        # 子图3: 每个核心的温度
        ax3 = axes[2]
        temp_columns = [col for col in data.keys() if 'Core' in col and 'Temp(C)' in col and col.startswith('Core')]
        has_core_temp = False
        for col in temp_columns[:16]:  # 限制16个核心
            # 检查是否有有效的温度数据（>0）
            try:
                values = data[col]
                if any(v > 0 for v in values):
                    ax3.plot(time, values, label=col.replace('_Temp(C)', ''), alpha=0.7)
                    has_core_temp = True
            except (TypeError, ValueError):
                pass
        
        ax3.set_title('Per-Core Temperature History')
        ax3.set_ylabel('Temperature (°C)')
        ax3.grid(True, alpha=0.3)
        if has_core_temp:
            ax3.legend(loc='best', fontsize=8)
        else:
            ax3.text(0.5, 0.5, 'No per-core temperature data available', 
                    ha='center', va='center', transform=ax3.transAxes,
                    bbox=dict(boxstyle='round', facecolor='lightyellow', alpha=0.8))
        
        if len(temp_columns) > 16:
            ax3.text(0.02, 0.95, f'Showing first 16 of {len(temp_columns)} cores', 
                    transform=ax3.transAxes, fontsize=9, verticalalignment='top',
                    bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
        
        # 子图4: 总体CPU温度
        ax4 = axes[3]
        if 'CPU_Temp(C)' in data:
            cpu_temp = data['CPU_Temp(C)']
            ax4.plot(time, cpu_temp, 'r-', linewidth=2, label='CPU Temp')
            ax4.set_title('Overall CPU Temperature History')
            ax4.set_ylabel('Temperature (°C)')
            ax4.grid(True, alpha=0.3)
            ax4.legend(loc='best')
        else:
            ax4.text(0.5, 0.5, 'No overall temperature data available', 
                    ha='center', va='center', transform=ax4.transAxes)
        
        ax4.set_xlabel('Time (s)')
        
        # 调整布局并保存
        plt.tight_layout()
        plt.savefig(output_file, dpi=100, bbox_inches='tight')
        print(f"Plot saved to: {output_file}")
        plt.close()
        
        return True
        
    except Exception as e:
        print(f"Error generating plot: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return False

if __name__ == '__main__':
    if len(sys.argv) < 2:
        csv_file = 'cpu_stress_result.csv'
        output_file = 'cpu_stress_result.png'
    else:
        csv_file = sys.argv[1]
        output_file = sys.argv[2] if len(sys.argv) > 2 else 'cpu_stress_result.png'
    
    if not os.path.exists(csv_file):
        print(f"Error: CSV file not found: {csv_file}", file=sys.stderr)
        sys.exit(1)
    
    success = plot_csv_data(csv_file, output_file)
    sys.exit(0 if success else 1)
