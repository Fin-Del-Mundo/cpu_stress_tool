# CPU Stress Test Monitor

这是一个功能完整的 CPU 压力测试监控工具，支持在指定 CPU 核心上进行压力测试，实时监测 CPU 使用率、频率和温度，并导出结果为 CSV 和图表。

## 系统要求

- **操作系统**: Linux（需要访问 `/proc/stat` 和 `/sys/` 系统接口）
- **CPU 架构**: x86_64 与 ARM64（aarch64）均支持，包括 NVIDIA Jetson/Thor 等平台
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

### 功耗控制 (`--power`)
通过**锁定 CPU 频率**来控制功耗（0-100%），默认 100%（最高频率）。这样可以在降低功耗的同时**保持频率稳定**，便于观测。
- 工作原理：`--power=P` 把 CPU 频率钉死在 `[最低频率, 最高频率]` 区间的 **P% 处**，并让核心满载运行。功耗由这个固定频率决定。
  - `--power=100`：频率锁定在最高值
  - `--power=70`：频率锁定在区间 70% 处（如范围 400-4000MHz 时约为 2920MHz）
  - `--power=30`：频率锁定在区间 30% 处（适合长期低功耗测试）
- **必须使用 `sudo` 运行**才能写入 cpufreq 接口完成锁频；否则无法降功耗（核心会满载且频率由系统调度，实际接近 100%）。
- 由于核心始终满载运行，`htop` 等工具会显示约 100% 的占用率，这是设计使然——降功耗是通过限制时钟频率实现的，而非降低利用率。

### 频率锁定机制
程序启动时按以下方式锁定频率，退出时（Ctrl+C）自动恢复原始设置：
- 主方案：将所有核心的 `scaling_min_freq` 与 `scaling_max_freq` 同时钉到目标频率（对 `acpi-cpufreq`、`intel_pstate` 等驱动通用）
- 兜底：若无法钉频，则尝试把 governor 设为 `performance`（仅能保证 100% 场景的最高频稳定）

### CPU 温度监控
实时监测并记录 CPU 温度，运行时显示当前温度和平均温度，最终报告温度统计数据。支持多种温度源：
- **x86**：优先 `coretemp`（`Core N` / `Package`），逻辑核心通过拓扑映射到物理核心
- **ARM / NVIDIA Thor**：`thermal_zone`（如 `cpu-thermal`）作为温度源
- 整体温度优先取 `Package` 温度（无则取各核心平均，再无则取 thermal_zone）

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
| `--power=PERCENT` | 将 CPU 频率锁定到区间的百分比处（0-100，默认 100，需 sudo）| `--power=70` |
| `--no-plot` | 跳过图表生成 | `--no-plot` |
| `--no-csv` | 跳过 CSV 导出 | `--no-csv` |
| `-h, --help` | 显示帮助信息 | `--help` |

## 使用示例

```bash
# 降功耗 + 频率稳定：必须用 sudo 才能锁频
sudo ./cpu_monitor --power=70

# 默认：所有核心锁定最高频率运行
sudo ./cpu_monitor

# 指定核心 + 锁频到 50%（图表只显示核心 0-3）
sudo ./cpu_monitor --cores=0-3 --power=50

# 不生成图表
sudo ./cpu_monitor --power=50 --no-plot
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
| CPU 频率跳变 / 无法降功耗 | 使用 `sudo` 运行，程序才能写入 cpufreq 接口锁频 |
| 频率锁定失败警告 | 检查是否以 `sudo` 运行；确认存在 `/sys/devices/system/cpu/cpu0/cpufreq/` 接口 |
| 温度读取失败 | 系统无温度传感器，不影响其他功能 |
| Ctrl+C 响应缓慢 | 多按几次或使用 `kill -INT <PID>` |
| matplotlib 图表失败 | 执行 `pip3 install matplotlib numpy` |

## 关于 CPU 频率稳定性

降低功耗的同时保持频率稳定，唯一物理可行的方法是：**把频率锁定在一个固定的中间值，再让核心满载运行**。早期基于"工作/休眠占空比"的方式无法做到这一点——休眠阶段核心真正空闲，频率必然掉到最低，导致频率在最低值与最高值之间剧烈跳变。

因此本工具的 `--power=P` 直接把频率钉死在 `[最低, 最高]` 区间的 P% 处：

- 频率在整个测试期间保持稳定，便于观测
- 功耗随固定频率线性下降
- **需要 `sudo`** 才能写入 cpufreq 接口；无权限时只能满载跑在系统调度的频率上（无法降功耗），程序会打印警告

---
详见 [CHANGELOG.md](CHANGELOG.md) 了解最新更新