# CPU Stress Test Monitor

这是一个功能完整的 CPU 压力测试监控工具，支持在指定 CPU 核心上进行压力测试，实时监测 CPU 使用率、频率和温度，并导出结果为 CSV 和图表。

## 系统要求

- **操作系统**: Linux（需要访问 `/proc/stat` 和 `/sys/` 系统接口）
- **编译器**: GCC/Clang，支持 C++11 或更高版本
- **CMake**: 3.10 或更高版本
- **Python 3**: 若要生成图表（可选）
- **matplotlib**: Python 库，若要生成图表（可选）

## 功能特性

### 指定核心压测 (`--cores`)
支持在指定的 CPU 核心上进行压力测试，通过 CPU 亲和性确保负载完全施加在选定核心。
- 列表形式：`--cores=0,1,2`
- 范围形式：`--cores=0-3`
- 混合形式：`--cores=0,2,4-6`

### 动态功率控制 (`--power`)
按百分比控制 CPU 功率（0-100%），默认 100% 满载。用于温控、功耗评估或长期稳定性测试。
- `--power=100`：CPU 满载运行
- `--power=50`：50% 功率（工作/休眠周期控制）
- `--power=30`：30% 功率（适合长期测试）

### CPU 温度监控
实时监测并记录 CPU 温度，支持多个温度源，运行时显示当前温度和平均温度，最终报告统计温度统计数据。

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
| `--power=PERCENT` | 设置 CPU 功率百分比（0-100，默认 100）| `--power=50` 或 `--power=75` |
| `--plot` | 生成 matplotlib 图表（需编译支持） | `--plot` |
| `--no-csv` | 跳过 CSV 导出 | `--no-csv` |
| `-h, --help` | 显示帮助信息 | `--help` |

### 常见用法

```bash
./build/cpu_monitor                                    # 默认：所有核心 100% 功率
./build/cpu_monitor --power=50                         # 50% 功率
./build/cpu_monitor --cores=0,1,2 --power=75 --plot   # 指定核心 + 功率 + 图表
./build/cpu_monitor --cores=0-4 --no-csv --plot       # 核心范围 + 不保存 CSV
./build/cpu_monitor --help                             # 显示帮助信息
```

## 输出文件

- `cpu_stress_result.csv` - 包含时间、各核心 CPU 使用率、频率、温度数据
- `cpu_stress_result.png` - 图表（使用 `--plot` 选项时生成）

## 编译和运行

### 编译
```bash
mkdir -p build && cd build
cmake -DENABLE_MATPLOTLIB=ON ..
cmake --build .
```

### 系统依赖
**Ubuntu/Debian：**
```bash
sudo apt-get install build-essential cmake python3 python3-matplotlib
```

**CentOS/RHEL：**
```bash
sudo yum groupinstall "Development Tools"
sudo yum install cmake python3 python3-matplotlib
```

## 项目文件

- `cpu_monitor.cpp` - 主程序源代码
- `CMakeLists.txt` - CMake 配置
- `plot_data.py` - 图表生成脚本
- `matplotlibcpp.h` - matplotlib C++ 头文件
- `CHANGELOG.md` - 更新日志

## 故障排除

| 问题 | 解决方案 |
|-----|--------|
| 温度读取失败 | 系统无温度传感器，不影响其他功能 |
| Ctrl+C 响应缓慢 | 多按几次或使用 `kill -INT <PID>` |
| matplotlib 图表失败 | 执行 `pip3 install matplotlib` |
| 权限不足 | 某些频率读取可能需要 root 权限 |

---
详见 [CHANGELOG.md](CHANGELOG.md) 了解最新更新
````


