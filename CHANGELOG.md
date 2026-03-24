# Changelog

## [2.1.0] - 2026-03-24

### Added
- **CPU Governor 自动管理**
  - 程序启动时自动将所有 CPU 核心的 governor 设置为 `performance` 模式
  - 程序退出时自动恢复原来的 governor 设置
  - 解决了 `powersave` 模式下 CPU 频率跳变的问题
  - 需要 sudo 权限，无权限时给出清晰的提示信息

- **纯 C++ matplotlib 绘图**
  - 使用 `matplotlibcpp.h` 在 C++ 中直接调用 matplotlib
  - 无需外部 Python 脚本 (`plot_data.py` 已删除)
  - 图表生成完全集成到主程序中

### Changed
- **图表显示逻辑优化**
  - 使用 `--cores` 指定核心时，图表只显示指定核心的数据
  - 不指定 `--cores` 时，显示所有核心数据（移除了 16 核限制）
  - 颜色循环使用，支持任意核心数量

- **压力测试周期优化**
  - 从 20ms 宏周期改为 1ms 微周期
  - 各线程工作周期错开，减少负载波动
  - 提高了功率控制的精确度

- **编译系统简化**
  - 移除 `ENABLE_MATPLOTLIB` 编译选项
  - 自动检测 Python3 和 NumPy 环境
  - 默认启用 matplotlibcpp 支持

### Removed
- `plot_data.py` - 不再需要外部 Python 脚本
- 核心数量显示限制（之前最多显示 16 核）

### Technical Details
- `stress_worker()` 使用 1ms 周期实现更精确的功率控制
- 遍历所有核心设置 governor，而非只设置 cpu0
- 使用 `plt::subplot2grid()` 替代 `plt::subplot()` 修复兼容性问题

### Usage Examples
```bash
# 使用 sudo 启用 performance governor（推荐）
sudo ./cpu_monitor --power=50

# 不使用 sudo（频率可能波动）
./cpu_monitor --power=50

# 只测试指定核心，图表只显示这些核心
./cpu_monitor --cores=0-3 --power=50

# 显示所有核心（无核心数量限制）
./cpu_monitor --power=50
```

---

## [2.0.0] - 2026-03-02

### Added
- **动态功率控制功能** (`--power=PERCENT`)
  - 支持设置 CPU 功率百分比（0-100%），默认 100%（满载）
  - 100% 功率：CPU 持续满载运行
  - 低功率模式：采用工作/休眠周期控制
  - 用例：长期稳定性测试、温度控制、功耗评估

### Changed
- 更新命令行帮助信息，新增 `--power` 参数说明和示例
- 程序启动时显示功率模式（"Full power (100%)" 或 "N% power"）
- 更新 README.md 的功能特性和使用示例部分

---

## [1.0.0] - Previous Release

- 初始版本：支持指定核心压测、CPU 使用率/频率/温度监控、CSV/图表导出