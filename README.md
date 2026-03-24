# CPU Stress Test Monitor

这是一个功能完整的 CPU 压力测试监控工具，支持在指定 CPU 核心上进行压力测试，实时监测 CPU 使用率、频率和温度，并导出结果为 CSV 和图表。

## 系统要求

- **操作系统**: Linux（需要访问 `/proc/stat` 和 `/sys/` 系统接口）
- **编译器**: GCC/Clang，支持 C++17 或更高版本
- **CMake**: 3.14 或更高版本
- **Python 3 + NumPy**: 图表生成依赖
- **matplotlib**: Python 库，图表生成依赖

## 功能特性

### 指定核心压测 (`--cores`)
支持在指定的 CPU 核心上进行压力测试，通过 CPU 亲和性确保负载完全施加在选定核心。
- 列表形式：`--cores=0,1,2`
- 范围形式：`--cores=0-3`
- 混合形式：`--cores=0,2,4-6`
- **图表只显示指定核心的数据**

### 动态功率控制 (`--power`)
按百分比控制 CPU 功率（0-100%），默认 100% 满载。用于温控、功耗评估或长期稳定性测试。
- `--power=100`：CPU 满载运行
- `--power=50`：50% 功率
- `--power=30`：30% 功率（适合长期测试）

### CPU Governor 自动管理
- 程序启动时自动将所有 CPU 核心设置为 `performance` 模式
- 程序退出时自动恢复原来的 governor 设置
- **推荐使用 `sudo` 运行以获得稳定的 CPU 频率**

### CPU 温度监控
实时监测并记录 CPU 温度，支持多个温度源，运行时显示当前温度和平均温度，最终报告统计温度统计数据。

### 纯 C++ 图表生成
- 使用 `matplotlibcpp.h` 直接在 C++ 中生成图表
- 无需外部 Python 脚本
- 支持任意核心数量（无限制）

## 编译步骤

```bash
# 安装依赖
sudo apt-get install build-essential cmake python3 python3-numpy python3-matplotlib

# 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译完成后，可执行文件位于 `build/cpu_monitor`

## 命令行选项

| 选项 | 说明 | 示例 |
|------|------|------|
| `--cores=LIST` | 指定要压测的 CPU 核心（图表只显示这些核心） | `--cores=0,1,2` 或 `--cores=0-3` |
| `--power=PERCENT` | 设置 CPU 功率百分比（0-100，默认 100）| `--power=50` |
| `--no-plot` | 跳过图表生成 | `--no-plot` |
| `--no-csv` | 跳过 CSV 导出 | `--no-csv` |
| `-h, --help` | 显示帮助信息 | `--help` |

## 使用示例

```bash
# 推荐方式：使用 sudo 启用 performance governor
sudo ./cpu_monitor --power=50

# 默认：所有核心 100% 功率
./cpu_monitor

# 指定核心 + 50% 功率（图表只显示核心 0-3）
./cpu_monitor --cores=0-3 --power=50

# 不生成图表
./cpu_monitor --power=50 --no-plot
```

## 输出文件

- `cpu_stress_result.csv` - 包含时间、各核心 CPU 使用率、频率、温度数据
- `cpu_stress_result.png` - 图表（1200x900 PNG）

## 项目文件

- `cpu_monitor.cpp` - 主程序源代码
- `CMakeLists.txt` - CMake 配置
- `matplotlibcpp.h` - matplotlib C++ 头文件
- `CHANGELOG.md` - 更新日志

## 故障排除

| 问题 | 解决方案 |
|-----|--------|
| CPU 频率跳变 | 使用 `sudo` 运行程序，启用 `performance` governor |
| 温度读取失败 | 系统无温度传感器，不影响其他功能 |
| Ctrl+C 响应缓慢 | 多按几次或使用 `kill -INT <PID>` |
| matplotlib 图表失败 | 执行 `pip3 install matplotlib numpy` |
| Governor 设置失败 | 使用 `sudo` 运行或手动执行 `sudo cpupower frequency-set -g performance` |

## 关于 CPU 频率稳定性

当使用 `--power` 参数时，CPU 频率稳定性取决于 CPU Governor 设置：

- **`performance` 模式**：频率锁定在最大值，稳定不变
- **`powersave` 模式**：频率随负载动态调整，会有跳变

程序会自动尝试设置为 `performance` 模式，但需要 `sudo` 权限。如果无法设置，会显示警告信息。

---
详见 [CHANGELOG.md](CHANGELOG.md) 了解最新更新