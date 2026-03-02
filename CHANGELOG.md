# Changelog

## [2.0.0] - 2026-03-02

### Added
- **动态功率控制功能** (`--power=PERCENT`)
  - 支持设置 CPU 功率百分比（0-100%），默认 100%（满载）
  - 100% 功率：CPU 持续满载运行
  - 低功率模式：采用工作/休眠周期控制（100ms 周期）
  - 用例：长期稳定性测试、温度控制、功耗评估

### Changed
- 更新命令行帮助信息，新增 `--power` 参数说明和示例
- 程序启动时显示功率模式（"Full power (100%)" 或 "N% power"）
- 更新 README.md 的功能特性和使用示例部分

### Technical Details
- 新增全局变量 `g_power_percent` 控制功率
- 修改 `stress_worker()` 函数实现功率控制逻辑
- 添加命令行参数 `--power=PERCENT` 的解析和验证（0-100 整数）
- 支持与 `--cores`、`--plot`、`--no-csv` 等参数组合使用

### Usage Examples
```bash
# 满载测试（默认）
./build/cpu_monitor

# 50% 功率运行
./build/cpu_monitor --power=50

# 指定核心 + 功率控制
./build/cpu_monitor --cores=0-3 --power=75 --plot

# 长期稳定性测试（低功率）
./build/cpu_monitor --power=40
```

### Backward Compatibility
- ✅ 完全向后兼容，所有现有命令仍然有效
- ✅ 默认行为不变（100% 功率满载）
- ✅ 新参数为可选项

---

## [1.0.0] - Previous Release

- 初始版本：支持指定核心压测、CPU 使用率/频率/温度监控、CSV/图表导出
