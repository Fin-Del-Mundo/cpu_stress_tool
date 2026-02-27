# CPU Stress Test Monitor

这是一个功能完整的 CPU 压力测试监控工具，支持在指定 CPU 核心上进行压力测试，实时监测 CPU 使用率、频率和温度，并导出结果为 CSV 和图表。

## 系统要求

- **操作系统**: Linux（需要访问 `/proc/stat` 和 `/sys/` 系统接口）
- **编译器**: GCC/Clang，支持 C++11 或更高版本
- **CMake**: 3.10 或更高版本
- **Python 3**: 若要生成图表（可选）
- **matplotlib**: Python 库，若要生成图表（可选）

## 功能特性

### 1. 指定核心压测 (`--cores`)
支持在指定的 CPU 核心上进行压力测试，通过 CPU 亲和性确保负载完全施加在选定核心。

**语法支持：**
- 列表形式：`--cores=0,1,2` - 压测核心 0, 1, 2
- 范围形式：`--cores=0-3` - 压测核心 0 到 3
- 混合形式：`--cores=0,2,4-6` - 压测核心 0, 2 以及 4 到 6

### 2. 动态 CPU 温度监控
实时监测并记录 CPU 温度，支持多个温度源：
- 优先使用 `/sys/class/hwmon/` 中的所有温度传感器（多核平均）
- 备选 `/sys/class/thermal/thermal_zone0/temp`
- 聚合多个传感器值，提供更准确的温度表示

**特点：**
- 运行时显示当前温度和运行平均温度
- CSV 导出包含完整温度历史
- 最终报告统计温度最大值、最小值、平均值

## 编译步骤

### 基本编译（无图表支持）

```bash
# 进入项目目录
cd /path/to/my_cpu_stress

# 创建 build 目录
mkdir -p build
cd build

# 配置并编译
cmake ..
cmake --build .
```

编译完成后，可执行文件位于 `build/cpu_monitor`

### 带 matplotlib 图表支持

如果系统已安装 Python 3 和 matplotlib，可启用图表生成：

```bash
cd build
cmake -DENABLE_MATPLOTLIB=ON ..
cmake --build .
```

**验证 matplotlib 支持：**
```bash
./cpu_monitor --help | grep matplotlib
```

## 完整命令行选项

| 选项 | 说明 | 示例 |
|------|------|------|
| `--cores=LIST` | 指定要压测的 CPU 核心 | `--cores=0,1,2` 或 `--cores=0-3` |
| `--plot` | 生成 matplotlib 图表（需编译支持） | `--plot` |
| `--no-csv` | 跳过 CSV 导出 | `--no-csv` |
| `-h, --help` | 显示帮助信息 | `--help` |

### 常见用法

```bash
# 使用所有核心进行压力测试，生成 CSV
./build/cpu_monitor

# 压测核心 0, 1, 2，并生成图表
./build/cpu_monitor --cores=0,1,2 --plot

# 压测核心 0-4，不生成 CSV，但生成图表
./build/cpu_monitor --cores=0-4 --no-csv --plot

# 压测核心 0, 3, 5-7（混合形式）
./build/cpu_monitor --cores=0,3,5-7

# 查看帮助
./build/cpu_monitor --help
```

### 交互操作

程序启动后会显示：
```
Detected 12 cores total.
Testing cores: 3, 7, 11
Starting stress test... Press [Ctrl+C] to stop and generate plots.
[Recording] Time: 12.5s | Temp: 65.3°C (avg 65.1°C)
```

按 **`Ctrl+C`** 停止测试，程序会自动生成报告和导出文件。

## 输出文件

### CSV 文件 (`cpu_stress_result.csv`)
包含以下列：
- `Time(s)` - 测试经过时间（秒）
- `Core0_Usage(%)`, `Core1_Usage(%)`, ... - 各核心 CPU 使用率（0-100%）
- `Core0_Freq(MHz)`, `Core1_Freq(MHz)`, ... - 各核心当前频率
- `Core0_Temp(C)`, `Core1_Temp(C)`, ... - 各核心温度（可选）
- `CPU_Temp(C)` - CPU 全局温度（来自 hwmon 传感器平均值）

### 图表文件 (`cpu_stress_result.png`)
由 matplotlib 生成的图表（使用 `--plot` 选项），包含以下内容：
1. **CPU 核心使用率历史** - 线图，展示每个核心的 CPU 使用率变化趋势
2. **CPU 核心频率历史** - 线图，展示每个核心的动态频率变化
3. **CPU 温度历史** - 线图，展示 CPU 温度的整体变化趋势

### 控制台输出
运行完成后，程序输出统计摘要：
```
=== CPU Stress Test Results ===
Total Duration: 215.9 seconds
Data Points Collected: 424

Temperature Statistics:
--------------------------------------------------
Average Temp:       63.5°C
Max Temp:           64.9°C
Min Temp:           60.5°C
--------------------------------------------------

Core-wise Statistics:
Core      Avg Usage (%)  Max Usage (%)  Min Usage (%)  Avg Freq (MHz)
0         99.78          100.00         7.84           4237.81
1         0.11           2.00           0.00           1130.23
...
```

## 编译需求详解

### Linux 系统依赖

**Ubuntu/Debian：**
```bash
# 安装必要的编译工具
sudo apt-get update
sudo apt-get install -y build-essential cmake python3 python3-matplotlib

# 验证版本
cmake --version
g++ --version
python3 --version
```

**CentOS/RHEL：**
```bash
sudo yum groupinstall "Development Tools"
sudo yum install cmake python3 python3-matplotlib
```

### 文件说明

- `cpu_monitor.cpp` - 主程序源代码
- `CMakeLists.txt` - CMake 配置文件
- `plot_data.py` - Python 脚本，用于生成图表
- `matplotlibcpp.h` - matplotlib C++ 头文件（用于图表生成）

## 故障排除

### 编译错误

**错误：找不到 cmake**
```bash
# Ubuntu/Debian
sudo apt-get install cmake

# 或手动下载：
# https://cmake.org/download/
```

**错误：找不到 pthread**
```bash
# 某些系统上可能需要显式链接，修改 CMakeLists.txt
# 将下面一行添加到 target_link_libraries
target_link_libraries(cpu_monitor pthread)
```

### 运行时错误

**温度读取失败**
- 原因：系统没有温度传感器支持
- 方案：程序会自动跳过温度读取，仅显示 0，不影响其他功能

**Ctrl+C 响应缓慢**
- 原因：信号处理需要时间
- 方案：多按几次 Ctrl+C，或使用 `kill -INT <PID>`

**matplotlib 图表生成失败**
```bash
# 检查 matplotlib 是否安装
python3 -c "import matplotlib; print(matplotlib.__version__)"

# 安装 matplotlib
pip3 install matplotlib
```

**权限不足**
```bash
# 某些系统调用需要权限，若需要完整的频率信息，可能需要 root
sudo ./build/cpu_monitor --cores=0,1
```

## 快速开始指南

### Step 1: 克隆或进入项目目录
```bash
cd /path/to/my_cpu_stress
```

### Step 2: 编译
```bash
mkdir -p build && cd build
cmake -DENABLE_MATPLOTLIB=ON ..
cmake --build .
```

### Step 3: 运行示例
```bash
# 压测核心 0, 1, 2，持续约 30 秒后按 Ctrl+C
./cpu_monitor --cores=0,1,2 --plot
```

### Step 4: 查看结果
```bash
# 查看 CSV 数据
cat ../cpu_stress_result.csv

# 查看生成的图表（需要图形查看器）
eog ../cpu_stress_result.png        # Linux
open ../cpu_stress_result.png       # macOS
```

## 项目架构

```
my_cpu_stress/
├── cpu_monitor.cpp           # 主程序：压力测试、数据采集、输出
├── plot_data.py              # 图表生成脚本
├── matplotlibcpp.h           # matplotlib C++ 接口头文件
├── CMakeLists.txt            # 构建配置
├── README.md                 # 本文件
└── build/                    # 编译输出目录
    └── cpu_monitor           # 可执行文件
```

## 技术细节

### CPU 亲和性（Affinity）
使用 `pthread_setaffinity_np()` 将每个压力测试线程绑定到指定的 CPU 核心，确保只有指定的核心满载运行。

### 温度监控策略
1. 优先从 `/sys/class/hwmon/` 目录读取所有可用的温度传感器
2. 收集有效的温度值（0-150°C 范围内）
3. 计算多个传感器的平均值，提供更准确的系统温度表示
4. 备选方案：使用 `/sys/class/thermal/thermal_zone0/temp`

### 数据采样
- 采样频率：500ms（2Hz）
- CPU 使用率：从 `/proc/stat` 读取并计算 delta
- 频率：从 `/sys/devices/system/cpu/cpuX/cpufreq/scaling_cur_freq` 读取
- 温度：实时采样，避免缓存值

## 许可证和贡献

本项目为开源工具，欢迎改进和贡献。


