#include <iostream>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <cmath>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <filesystem>
#include <map>
#include <numeric>
#include <deque>

#ifdef ENABLE_MATPLOTLIB_CPP
#include "matplotlibcpp.h"  // C++ matplotlib 绑定
namespace plt = matplotlibcpp;
#endif

volatile std::sig_atomic_t g_stop = 0;
static int g_power_percent = 100;  // 全局功率百分比 (0-100): 通过锁定 CPU 频率到该百分比来控制功耗

void signal_handler(int signal) {
    g_stop = 1;
}

// --- 压力测试模块 ---
// worker 始终满载运行。功耗由锁定的 CPU 频率决定（见 main 中的频率控制逻辑）。
void stress_worker(int core_id, int total_threads, int thread_index) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t thread = pthread_self();
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    while (g_stop == 0) {
        double x = 0.1;
        for (int i = 0; i < 5000; ++i) {
            x = std::sin(x) * std::cos(x) + std::tan(x);
        }
    }
}

// --- 数据读取模块 ---
struct CpuRawData {
    unsigned long long active;
    unsigned long long total;
};

struct CoreConfig {
    std::vector<int> cores;  // 指定要测试的核心
    bool use_all_cores = true;  // 默认使用所有核心
};

std::vector<CpuRawData> read_cpu_stats() {
    std::ifstream file("/proc/stat");
    std::string line;
    std::vector<CpuRawData> stats;

    while (std::getline(file, line)) {
        if (line.substr(0, 3) == "cpu" && line.size() > 3 && isdigit(line[3])) {
            std::stringstream ss(line);
            std::string label;
            unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
            ss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
            
            CpuRawData data;
            data.active = user + nice + system + irq + softirq + steal;
            data.total = data.active + idle + iowait;
            stats.push_back(data);
        }
    }
    return stats;
}

double get_cpu_freq_mhz(int core_id) {
    std::string cpu_base = "/sys/devices/system/cpu/cpu" + std::to_string(core_id) + "/cpufreq/";
    // 不同平台/驱动暴露的频率节点不同, 依次尝试:
    //   scaling_cur_freq : 通用 (governor 请求频率)
    //   cpuinfo_cur_freq : 实际硬件频率 (部分平台需 root)
    // NVIDIA Tegra/Thor 在 cpufreq 不可用时, 频率位于 policy 节点
    std::vector<std::string> candidates = {
        cpu_base + "scaling_cur_freq",
        cpu_base + "cpuinfo_cur_freq",
        "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(core_id) + "/scaling_cur_freq",
        "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(core_id) + "/cpuinfo_cur_freq",
    };

    for (const auto& path : candidates) {
        std::ifstream file(path);
        double freq_khz = 0;
        if (file.is_open() && (file >> freq_khz) && freq_khz > 0) {
            return freq_khz / 1000.0;  // cpufreq 单位为 kHz
        }
    }
    return 0.0;
}

// 温度平滑处理器
class TemperatureSmoother {
private:
    double smoothed_temp_ = 0.0;
    bool initialized_ = false;
    std::deque<double> history_;
    int window_size_ = 5;
    double alpha_ = 0.3;  // EMA 系数
    double max_jump_ = 15.0;  // 最大单次跳变限制

public:
    double smooth(double raw_temp) {
        if (raw_temp <= 0) return smoothed_temp_;

        if (!initialized_) {
            smoothed_temp_ = raw_temp;
            history_.push_back(raw_temp);
            initialized_ = true;
            return smoothed_temp_;
        }

        // 异常值限制
        double temp_diff = raw_temp - smoothed_temp_;
        double limited_temp;
        if (std::abs(temp_diff) > max_jump_) {
            double sign = (temp_diff > 0) ? 1.0 : -1.0;
            limited_temp = smoothed_temp_ + sign * max_jump_;
        } else {
            limited_temp = raw_temp;
        }

        // 更新滑动窗口
        history_.push_back(limited_temp);
        if (history_.size() > static_cast<size_t>(window_size_)) {
            history_.pop_front();
        }

        // 计算窗口平均值
        double window_avg = 0.0;
        if (!history_.empty()) {
            window_avg = std::accumulate(history_.begin(), history_.end(), 0.0) / history_.size();
        }

        // 结合 EMA 和滑动窗口平均
        smoothed_temp_ = alpha_ * window_avg + (1.0 - alpha_) * smoothed_temp_;
        return smoothed_temp_;
    }
};

// 全局温度平滑器
static TemperatureSmoother g_temp_smoother;

// 检查 hwmon 驱动名称是否为 CPU 温度相关
static bool is_cpu_hwmon_driver(const std::string& name) {
    static const std::vector<std::string> cpu_drivers = {
        "coretemp",       // x86 Intel
        "k10temp",        // x86 AMD
        "cpu_thermal",    // ARM 通用
        "nct6775",        // Nuvoton (some x86 boards)
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& d : cpu_drivers) {
        if (lower.find(d) != std::string::npos) return true;
    }
    return false;
}

// 通过 hwmon 驱动获取各核心/传感器温度
// x86: coretemp 驱动, 标签如 "Core 0", "Package id 0"
// ARM: cpu_thermal 等驱动, 标签可能为 "CPU-therm" 或传感器编号
std::map<std::string, double> get_core_temperatures() {
    std::map<std::string, double> core_temps;
    namespace fs = std::filesystem;
    std::string hwmon_base = "/sys/class/hwmon";

    try {
        if (!fs::exists(hwmon_base)) return core_temps;

        for (const auto& hwmon_entry : fs::directory_iterator(hwmon_base)) {
            if (!hwmon_entry.is_directory()) continue;

            std::string hwmon_dir = hwmon_entry.path().string();
            std::string name_file = hwmon_dir + "/name";

            if (!fs::exists(name_file)) continue;

            std::ifstream name_stream(name_file);
            std::string driver_name;
            if (!std::getline(name_stream, driver_name)) continue;
            driver_name.erase(driver_name.find_last_not_of(" \n\r\t") + 1);

            if (!is_cpu_hwmon_driver(driver_name)) continue;

            for (const auto& temp_entry : fs::directory_iterator(hwmon_dir)) {
                std::string entry_name = temp_entry.path().filename().string();

                // 优先使用 temp*_label + temp*_input 组合
                if (entry_name.find("temp") == 0 &&
                    entry_name.find("_label") != std::string::npos) {

                    std::string label_file = temp_entry.path().string();
                    std::ifstream label_stream(label_file);
                    std::string label;

                    if (std::getline(label_stream, label)) {
                        label.erase(label.find_last_not_of(" \n\r\t") + 1);

                        std::string input_file = label_file;
                        size_t label_pos = input_file.find("_label");
                        if (label_pos != std::string::npos) {
                            input_file.replace(label_pos, 6, "_input");

                            if (fs::exists(input_file)) {
                                std::ifstream input_stream(input_file);
                                int64_t raw_temp = 0;
                                if (input_stream >> raw_temp) {
                                    double temp_c = raw_temp / 1000.0;
                                    if (temp_c > 0 && temp_c < 150) {
                                        core_temps[label] = temp_c;
                                    }
                                }
                            }
                        }
                    }
                }

                // ARM 平台可能没有 _label 文件，直接读 temp*_input
                if (entry_name.find("temp") == 0 &&
                    entry_name.find("_input") != std::string::npos &&
                    entry_name.find("_label") == std::string::npos) {

                    // 检查对应的 _label 是否存在，已有则跳过（上面已处理）
                    std::string label_check = temp_entry.path().string();
                    size_t input_pos = label_check.find("_input");
                    if (input_pos != std::string::npos) {
                        std::string corresponding_label = label_check;
                        corresponding_label.replace(input_pos, 6, "_label");
                        if (fs::exists(corresponding_label)) continue;
                    }

                    std::ifstream input_stream(temp_entry.path().string());
                    int64_t raw_temp = 0;
                    if (input_stream >> raw_temp) {
                        double temp_c = raw_temp / 1000.0;
                        if (temp_c > 0 && temp_c < 150) {
                            // 用驱动名+传感器编号作为标签
                            std::string synth_label = driver_name + "/" + entry_name;
                            synth_label = synth_label.substr(0, synth_label.find("_input"));
                            core_temps[synth_label] = temp_c;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        // 静默处理错误
    }

    return core_temps;
}

// 检查 thermal_zone type 是否为 CPU 温度类型
static bool is_cpu_temp_type(const std::string& type) {
    // 常见的 CPU 温度类型标识
    static const std::vector<std::string> cpu_types = {
        "x86_pkg_temp",   // x86 CPU 包温度
        "coretemp",       // coretemp 驱动
        "cpu-therm",      // NVIDIA Jetson/Thor CPU 温度
        "cpu_thermal",    // ARM CPU 热传感器
        "tdiode_cpu",     // NVIDIA Tegra CPU diode
        "tboard_cpu",     // NVIDIA board-level CPU temp
        "soc_thermal",    // SoC 热传感器
        "soc-therm",      // NVIDIA SoC 热传感器
        "cpu",            // 通用 CPU 标识 (放后面避免误匹配 "gpu" 等)
    };

    std::string lower_type = type;
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(), ::tolower);

    for (const auto& cpu_type : cpu_types) {
        if (lower_type.find(cpu_type) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 从 thermal_zone 获取 CPU 温度（备用方案）
static double get_cpu_temp_from_thermal_zone() {
    namespace fs = std::filesystem;
    std::string tz_base = "/sys/class/thermal";

    try {
        if (fs::exists(tz_base)) {
            for (const auto& tz_entry : fs::directory_iterator(tz_base)) {
                if (!tz_entry.is_directory()) continue;

                std::string tz_dir = tz_entry.path().string();
                std::string type_file = tz_dir + "/type";
                std::string temp_file = tz_dir + "/temp";

                if (fs::exists(type_file) && fs::exists(temp_file)) {
                    std::ifstream type_stream(type_file);
                    std::string type;
                    if (std::getline(type_stream, type)) {
                        type.erase(type.find_last_not_of(" \n\r\t") + 1);

                        if (is_cpu_temp_type(type)) {
                            std::ifstream temp_stream(temp_file);
                            int64_t raw_temp = 0;
                            if (temp_stream >> raw_temp) {
                                double temp_c = raw_temp / 1000.0;
                                if (temp_c > 0 && temp_c < 150) {
                                    return temp_c;
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception&) {
        // 静默处理
    }

    return 0.0;
}

// 获取 CPU 整体温度 (单位: 摄氏度)
// 整体温度的语义是"整颗 CPU 的温度"。Package 温度本就代表整颗芯片, 应优先采用;
// 不能把 Package 温度和各核心温度混在一起平均(会把两种不同含义的测量值搅在一起,
// 且 Package 通常偏高, 拉高整体读数)。
double get_cpu_temp() {
    auto core_temps = get_core_temperatures();

    double result = 0.0;

    if (!core_temps.empty()) {
        // 优先: Package 温度 (多路 CPU 时取各 Package 平均)
        std::vector<double> package_temps;
        std::vector<double> per_core_temps;
        for (const auto& [label, temp] : core_temps) {
            if (temp <= 0 || temp >= 150) continue;
            if (label.find("Package") != std::string::npos) {
                package_temps.push_back(temp);
            } else if (label.find("Core") != std::string::npos) {
                per_core_temps.push_back(temp);
            } else {
                // ARM 等合成标签, 归入 per_core 作为兜底来源
                per_core_temps.push_back(temp);
            }
        }

        if (!package_temps.empty()) {
            result = std::accumulate(package_temps.begin(), package_temps.end(), 0.0) / package_temps.size();
        } else if (!per_core_temps.empty()) {
            result = std::accumulate(per_core_temps.begin(), per_core_temps.end(), 0.0) / per_core_temps.size();
        }
    }

    // 兜底: thermal_zone
    if (result <= 0) {
        double tz_temp = get_cpu_temp_from_thermal_zone();
        if (tz_temp > 0) result = tz_temp;
    }

    if (result > 0) {
        return g_temp_smoother.smooth(result);
    }
    return 0.0;
}

// 逻辑 CPU -> 物理核心 ID 映射
// coretemp 的 "Core N" 标签使用的是物理核心 ID, 而非逻辑 CPU 编号。
// 超线程下逻辑 CPU 0/1 可能同属物理 Core 0, 必须用 topology/core_id 做映射。
int get_physical_core_id(int cpu_id) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/topology/core_id";
    std::ifstream file(path);
    int id = -1;
    if (file.is_open()) file >> id;
    return id;
}

// 获取单个核心的温度 (单位: 摄氏度)
double get_core_temp(int core_id) {
    auto core_temps = get_core_temperatures();

    if (!core_temps.empty()) {
        // x86: 把逻辑 CPU 映射到物理核心 ID 后匹配 "Core N" 标签
        int phys = get_physical_core_id(core_id);
        if (phys >= 0) {
            auto it = core_temps.find("Core " + std::to_string(phys));
            if (it != core_temps.end()) return it->second;
        }

        // 兜底1: 直接用逻辑编号匹配 (拓扑信息不可用时)
        auto it = core_temps.find("Core " + std::to_string(core_id));
        if (it != core_temps.end()) return it->second;

        // 兜底2: Package 温度 (x86)
        for (const auto& [label, temp] : core_temps) {
            if (label.find("Package") != std::string::npos) return temp;
        }

        // 兜底3: 第一个有效温度 (ARM 合成标签)
        return core_temps.begin()->second;
    }

    // 兜底4: hwmon 无 CPU 温度 (NVIDIA Thor/Tegra 等) -> 使用 thermal_zone 的 CPU 温度
    // ARM 平台通常没有逐核心传感器, 各核心共享同一 SoC/CPU 温度
    return get_cpu_temp_from_thermal_zone();
}

// 解析核心列表，例如: "0,1,2" 或 "0-3"
std::vector<int> parse_cores(const std::string& cores_str, int max_cores) {
    std::vector<int> result;
    std::stringstream ss(cores_str);
    std::string token;
    
    while (std::getline(ss, token, ',')) {
        // 检查是否包含 "-" (范围表示)
        size_t dash_pos = token.find('-');
        if (dash_pos != std::string::npos) {
            int start = std::stoi(token.substr(0, dash_pos));
            int end = std::stoi(token.substr(dash_pos + 1));
            for (int i = start; i <= end && i < max_cores; ++i) {
                result.push_back(i);
            }
        } else {
            int core = std::stoi(token);
            if (core < max_cores) {
                result.push_back(core);
            }
        }
    }
    
    // 去重
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    
    return result;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    bool enable_plot = true;  // 默认生成图像
    bool enable_csv = true;
    CoreConfig core_config;
    std::string cores_str;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--no-plot") enable_plot = false;
        if (arg == "--no-csv") enable_csv = false;
        if (arg.find("--cores=") == 0) {
            cores_str = arg.substr(8);  // 提取 "--cores=" 后的内容
            core_config.use_all_cores = false;
        }
        if (arg.find("--power=") == 0) {
            std::string power_str = arg.substr(8);
            try {
                int power = std::stoi(power_str);
                if (power >= 0 && power <= 100) {
                    g_power_percent = power;
                } else {
                    std::cerr << "Error: Power percentage must be between 0 and 100!\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Error: Invalid power value!\n";
                return 1;
            }
        }
    }

    // 输出使用帮助
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: " << argv[0] << " [options]\n"
                  << "Options:\n"
                  << "  --cores=LIST         Specify CPU cores to stress test (e.g., --cores=0,1,2 or --cores=0-3)\n"
                  << "  --power=PERCENT      Set CPU power usage percentage (0-100, default: 100 for full power)\n"
                  << "  --no-plot            Skip plot generation\n"
                  << "  --no-csv             Skip CSV export\n"
                  << "  --help, -h           Show this help message\n"
                  << "\nHow --power works:\n"
                  << "  --power=P pins the CPU frequency at P% of the [min,max] range and runs full load.\n"
                  << "  Frequency stays STABLE; power consumption scales with the fixed frequency.\n"
                  << "  Requires root (sudo) to write cpufreq sysfs. htop will show ~100% utilization by\n"
                  << "  design (the cores are fully busy, only their clock speed is capped).\n"
                  << "\nExamples:\n"
                  << "  sudo " << argv[0] << "                     # All cores at 100% (max frequency)\n"
                  << "  sudo " << argv[0] << " --power=70           # Pin freq at 70% of range, stable freq\n"
                  << "  sudo " << argv[0] << " --power=50           # Pin freq at 50% of range\n"
                  << "  sudo " << argv[0] << " --cores=0-3 --power=75 # Cores 0-3, freq pinned at 75%\n";
        return 0;
    }

    // 注册 Ctrl+C 信号
    std::signal(SIGINT, signal_handler);

    int num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 4;
    
    // 设置要测试的核心
    std::vector<int> test_cores;
    if (core_config.use_all_cores) {
        for (int i = 0; i < num_cores; ++i) {
            test_cores.push_back(i);
        }
    } else {
        test_cores = parse_cores(cores_str, num_cores);
        if (test_cores.empty()) {
            std::cerr << "Error: No valid cores specified!\n";
            return 1;
        }
    }

    // --- 频率控制 ---
    // 要在降功耗的同时保持频率稳定, 唯一物理可行的方法是: 把频率钉在一个固定的中间值,
    // 然后让核心满载运行。--power=P 即把频率锁定到 [min,max] 区间的 P% 处。
    auto read_sysfs = [](const std::string& path) -> std::string {
        std::ifstream f(path);
        std::string val;
        if (f.is_open() && std::getline(f, val)) {
            val.erase(val.find_last_not_of(" \n\r\t") + 1);
        }
        return val;
    };

    auto write_sysfs = [](const std::string& path, const std::string& val) -> bool {
        std::ofstream f(path);
        if (f.is_open()) {
            f << val;
            f.close();
            return f.good();
        }
        return false;
    };

    // 恢复用的原始状态
    std::string original_governor;
    std::vector<std::string> original_min_freqs(num_cores);
    std::vector<std::string> original_max_freqs(num_cores);
    bool governor_changed = false;
    bool freq_capped = false;       // 修改了 scaling_min/max_freq

    // 读取硬件频率范围
    long fmin_khz = 0, fmax_khz = 0;
    {
        std::string s_min = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
        std::string s_max = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        try { if (!s_min.empty()) fmin_khz = std::stol(s_min); } catch (...) {}
        try { if (!s_max.empty()) fmax_khz = std::stol(s_max); } catch (...) {}
    }

    bool cpufreq_available = (fmin_khz > 0 && fmax_khz > 0);

    // 决定目标频率: power=100 -> 最大频率; 否则 -> 区间内 power% 处
    long target_khz = fmax_khz;
    if (g_power_percent < 100 && cpufreq_available) {
        target_khz = fmin_khz + (long)((fmax_khz - fmin_khz) * (double)g_power_percent / 100.0);

        // 若有可用频率档位列表, 吸附到最接近的合法档位
        std::string avail = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies");
        if (!avail.empty()) {
            std::stringstream ss(avail);
            std::string tok;
            long best = target_khz, best_diff = -1;
            while (ss >> tok) {
                try {
                    long f = std::stol(tok);
                    long d = std::labs(f - target_khz);
                    if (best_diff < 0 || d < best_diff) { best_diff = d; best = f; }
                } catch (...) {}
            }
            target_khz = best;
        }
    }

    // 保存当前 governor
    original_governor = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");

    bool freq_lock_mode = (g_power_percent < 100 && cpufreq_available);

    if (cpufreq_available) {
        // 把 scaling_min_freq 与 scaling_max_freq 同时钉到 target, 强制锁频
        // 这是跨驱动通用方案: 对 acpi-cpufreq / intel_pstate 都有效
        // 注意: scaling_max_freq 本身已对频率封顶(含 turbo 范围内), 无需也不应关闭 turbo,
        //       否则会把频率限制在基础频率, 导致 100% 时上不去最高频。
        bool all_ok = true;
        for (int i = 0; i < num_cores; ++i) {
            std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
            original_min_freqs[i] = read_sysfs(base + "scaling_min_freq");
            original_max_freqs[i] = read_sysfs(base + "scaling_max_freq");
            // 先抬 max 再压 min, 避免 min>max 的写入被拒
            bool ok1 = write_sysfs(base + "scaling_max_freq", std::to_string(target_khz));
            bool ok2 = write_sysfs(base + "scaling_min_freq", std::to_string(target_khz));
            if (!ok1 || !ok2) all_ok = false;
        }

        // 验证: 驱动可能把写入值吸附到最近的合法档位, 因此只要 min==max(钉到单点)即视为成功
        long rmin = 0, rmax = 0;
        try { rmin = std::stol(read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq")); } catch (...) {}
        try { rmax = std::stol(read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq")); } catch (...) {}

        if (all_ok && rmin > 0 && rmin == rmax) {
            freq_capped = true;
            long actual_mhz = rmin / 1000;  // 实际生效频率(可能被驱动吸附)
            if (freq_lock_mode) {
                std::cout << "[Freq Lock] Pinned to " << actual_mhz << " MHz "
                          << "(range " << fmin_khz / 1000 << "-" << fmax_khz / 1000 << " MHz, "
                          << "target " << g_power_percent << "%), running FULL load.\n";
            } else {
                std::cout << "[Freq Lock] Pinned to " << actual_mhz << " MHz (max).\n";
            }
        }
    }

    // 频率钉死失败时, 尝试 governor=performance 作为兜底 (至少 power=100 场景稳定)
    if (!freq_capped && original_governor != "performance") {
        bool all_ok = true;
        for (int i = 0; i < num_cores; ++i) {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
            if (!write_sysfs(path, "performance")) all_ok = false;
        }
        if (all_ok && read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") == "performance") {
            governor_changed = true;
            std::cout << "[Freq Lock] Governor -> performance (freq pin unavailable)\n";
        }
    }

    if (!freq_capped && !governor_changed && original_governor != "performance") {
        std::cout << "\n"
                  << "WARNING: Failed to control CPU frequency (need root). Frequency WILL fluctuate.\n"
                  << "  Run with: sudo " << argv[0] << " --power=" << g_power_percent << "\n"
                  << "\n";
    }
    if (freq_lock_mode && !freq_capped) {
        std::cout << "Note: Could not pin frequency (need root). Cores will run at full load and the\n"
                  << "      governor will drive frequency freely -- effective power will be ~100%, not "
                  << g_power_percent << "%.\n";
    }

    std::cout << "Detected " << num_cores << " cores total.\n";
    std::cout << "Testing cores: ";
    for (int i = 0; i < test_cores.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << test_cores[i];
    }
    std::cout << "\n";
    if (g_power_percent == 100) {
        std::cout << "Power mode: Full power (100%)\n";
    } else if (freq_capped) {
        std::cout << "Power mode: " << g_power_percent << "% (frequency-lock, stable freq, full load)\n";
    } else {
        std::cout << "Power mode: " << g_power_percent << "% requested, but frequency lock unavailable "
                  << "(running full load at governor-driven frequency)\n";
    }
    std::cout << "Starting stress test... Press [Ctrl+C] to stop and generate plots.\n";

    // 数据存储容器 (用于 Matplotlib 绘图)
    std::vector<double> time_log;
    std::vector<std::vector<double>> usage_logs(num_cores);
    std::vector<std::vector<double>> freq_logs(num_cores);
    std::vector<std::vector<double>> core_temp_logs(num_cores);  // 每个核心的温度日志
    std::vector<double> temp_logs;  // 全局 CPU 温度日志

    // 运行时统计平均温度，方便实时显示
    double running_avg_temp = 0.0;
    int temp_count = 0;

    // 启动压力线程 (仅在指定的核心上运行)
    std::vector<std::thread> threads;
    int total_threads = test_cores.size();
    for (int i = 0; i < total_threads; ++i) {
        int core_id = test_cores[i];
        // stress_worker 会将线程绑定到 core_id
        // 传入 total_threads 和线程索引 i 用于错开工作周期
        threads.emplace_back(stress_worker, core_id, total_threads, i);
    }

    auto prev_stats = read_cpu_stats();
    auto start_time = std::chrono::steady_clock::now();

    // --- 数据采集循环 ---
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto curr_stats = read_cpu_stats();
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();

        time_log.push_back(elapsed);
        
        // 获取 CPU 温度
        double cpu_temp = get_cpu_temp();
        temp_logs.push_back(cpu_temp);
        if (cpu_temp > 0) {
            running_avg_temp = (running_avg_temp * temp_count + cpu_temp) / (temp_count + 1);
            temp_count++;
        }
        
        std::cout << "\r[Recording] Time: " << elapsed << "s | Temp: " << std::fixed << std::setprecision(1) << cpu_temp << "°C";
        if (temp_count > 0) {
            std::cout << " (avg " << std::fixed << std::setprecision(1) << running_avg_temp << "°C)";
        }
        std::cout << " " << std::flush;

        for (int i = 0; i < num_cores; ++i) {
            // 计算占用率
            double usage = 0.0;
            if (i < prev_stats.size() && i < curr_stats.size()) {
                auto active_delta = curr_stats[i].active - prev_stats[i].active;
                auto total_delta = curr_stats[i].total - prev_stats[i].total;
                if (total_delta > 0) {
                    usage = 100.0 * static_cast<double>(active_delta) / total_delta;
                }
            }
            usage_logs[i].push_back(usage);

            // 获取频率
            double freq = get_cpu_freq_mhz(i);
            freq_logs[i].push_back(freq);
            
            // 获取该核心的温度
            double core_temp = get_core_temp(i);
            core_temp_logs[i].push_back(core_temp);
        }
        prev_stats = curr_stats;
    }

    // --- 停止线程 ---
    std::cout << "\nStopping stress test...\n";
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    // --- 统计和报告 ---
    std::cout << "\n=== CPU Stress Test Results ===\n";
    std::cout << "Total Duration: " << (time_log.empty() ? 0 : time_log.back()) << " seconds\n";
    std::cout << "Data Points Collected: " << time_log.size() << "\n";
    
    // 显示温度统计
    if (!temp_logs.empty()) {
        double avg_temp = 0.0, max_temp = -273.0, min_temp = 200.0;
        int valid_count = 0;
        for (double t : temp_logs) {
            if (t > 0) {
                avg_temp += t;
                max_temp = std::max(max_temp, t);
                min_temp = std::min(min_temp, t);
                valid_count++;
            }
        }
        // 如果全局温度一直读取为 0，尝试使用核心温度作为备用
        if (valid_count == 0) {
            for (int i = 0; i < num_cores; ++i) {
                for (double t : core_temp_logs[i]) {
                    if (t > 0) {
                        avg_temp += t;
                        max_temp = std::max(max_temp, t);
                        min_temp = std::min(min_temp, t);
                        valid_count++;
                    }
                }
            }
        }
        if (valid_count > 0) avg_temp /= valid_count;
        
        std::cout << "\nTemperature Statistics:\n";
        std::cout << std::string(50, '-') << "\n";
        std::cout << std::fixed << std::setprecision(1)
                  << std::left << std::setw(20) << "Average Temp:" << avg_temp << "°C\n"
                  << std::setw(20) << "Max Temp:" << max_temp << "°C\n"
                  << std::setw(20) << "Min Temp:" << min_temp << "°C\n";
        std::cout << std::string(50, '-') << "\n";
    }
    
    std::cout << "\nCore-wise Statistics:\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(10) << "Core" 
              << std::setw(15) << "Avg Usage (%)" 
              << std::setw(15) << "Max Usage (%)" 
              << std::setw(15) << "Min Usage (%)"
              << std::setw(15) << "Avg Freq (MHz)" << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (int i = 0; i < num_cores; ++i) {
        double avg_usage = 0.0, max_usage = 0.0, min_usage = 100.0;
        double avg_freq = 0.0;
        
        if (!usage_logs[i].empty()) {
            for (double u : usage_logs[i]) {
                avg_usage += u;
                max_usage = std::max(max_usage, u);
                min_usage = std::min(min_usage, u);
            }
            avg_usage /= usage_logs[i].size();
        }
        
        if (!freq_logs[i].empty()) {
            for (double f : freq_logs[i]) avg_freq += f;
            avg_freq /= freq_logs[i].size();
        }

        std::cout << std::left << std::setw(10) << i 
                  << std::fixed << std::setprecision(2)
                  << std::setw(15) << avg_usage
                  << std::setw(15) << max_usage
                  << std::setw(15) << min_usage
                  << std::setw(15) << avg_freq << "\n";
    }
    std::cout << std::string(80, '-') << "\n";

    // --- CSV 导出 ---
    if (enable_csv) {
        std::string csv_filename = "cpu_stress_result.csv";
        std::cout << "\nExporting data to " << csv_filename << "...\n";
        std::ofstream csv_file(csv_filename);
        
        // 写入 CSV 头部
        csv_file << "Time(s)";
        for (int i = 0; i < num_cores; ++i) {
            csv_file << ",Core" << i << "_Usage(%)";
        }
        for (int i = 0; i < num_cores; ++i) {
            csv_file << ",Core" << i << "_Freq(MHz)";
        }
        for (int i = 0; i < num_cores; ++i) {
            csv_file << ",Core" << i << "_Temp(C)";
        }
        csv_file << ",CPU_Temp(C)";
        csv_file << "\n";
        
        // 写入数据
        for (size_t t = 0; t < time_log.size(); ++t) {
            csv_file << std::fixed << std::setprecision(3) << time_log[t];
            for (int i = 0; i < num_cores; ++i) {
                if (t < usage_logs[i].size()) {
                    csv_file << "," << std::fixed << std::setprecision(2) << usage_logs[i][t];
                }
            }
            for (int i = 0; i < num_cores; ++i) {
                if (t < freq_logs[i].size()) {
                    csv_file << "," << std::fixed << std::setprecision(1) << freq_logs[i][t];
                }
            }
            for (int i = 0; i < num_cores; ++i) {
                if (t < core_temp_logs[i].size()) {
                    csv_file << "," << std::fixed << std::setprecision(1) << core_temp_logs[i][t];
                }
            }
            if (t < temp_logs.size()) {
                csv_file << "," << std::fixed << std::setprecision(1) << temp_logs[t];
            }
            csv_file << "\n";
        }
        csv_file.close();
        std::cout << "CSV file saved successfully.\n";
    }

    // --- matplotlibcpp 绘图 ---
#ifdef ENABLE_MATPLOTLIB_CPP
    if (enable_plot && !time_log.empty()) {
        std::cout << "\nGenerating plot with matplotlib...\n";

        try {
            // 设置非交互式后端（必须在其他 matplotlib 调用之前）
            plt::backend("Agg");

            // 创建图形
            plt::figure_size(1200, 900);

            // 颜色循环（会循环使用）
            std::vector<std::string> colors = {"C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9"};

            // 确定要显示的核心：如果指定了 --cores 就只显示指定的，否则显示全部
            std::vector<int> cores_to_display;
            if (core_config.use_all_cores) {
                for (int i = 0; i < num_cores; ++i) cores_to_display.push_back(i);
            } else {
                cores_to_display = test_cores;
            }

            // 子图1: CPU 使用率 (使用 subplot2grid 替代 subplot)
            plt::subplot2grid(4, 1, 0, 0);
            for (size_t idx = 0; idx < cores_to_display.size(); ++idx) {
                int core_id = cores_to_display[idx];
                if (core_id < num_cores && !usage_logs[core_id].empty()) {
                    plt::named_plot("Core " + std::to_string(core_id), time_log, usage_logs[core_id], colors[idx % colors.size()]);
                }
            }
            plt::title("CPU Core Usage History");
            plt::ylabel("Usage (%)");
            plt::ylim(-5.0, 105.0);
            plt::legend();
            plt::grid(true);

            // 子图2: CPU 频率
            plt::subplot2grid(4, 1, 1, 0);
            double freq_max = 0;
            for (int core_id : cores_to_display) {
                if (core_id < num_cores) {
                    for (double f : freq_logs[core_id]) freq_max = std::max(freq_max, f);
                }
            }
            freq_max = std::max(freq_max, 1000.0);
            for (size_t idx = 0; idx < cores_to_display.size(); ++idx) {
                int core_id = cores_to_display[idx];
                if (core_id < num_cores && !freq_logs[core_id].empty()) {
                    plt::named_plot("Core " + std::to_string(core_id), time_log, freq_logs[core_id], colors[idx % colors.size()]);
                }
            }
            plt::title("CPU Core Frequency History");
            plt::ylabel("Frequency (MHz)");
            plt::ylim(0.0, freq_max);
            plt::legend();
            plt::grid(true);

            // 子图3: 核心温度
            plt::subplot2grid(4, 1, 2, 0);
            double temp_min = 200, temp_max = 0;
            for (int core_id : cores_to_display) {
                if (core_id < num_cores) {
                    for (double t : core_temp_logs[core_id]) {
                        if (t > 0) { temp_min = std::min(temp_min, t); temp_max = std::max(temp_max, t); }
                    }
                }
            }
            if (temp_max == 0) { temp_min = 20; temp_max = 100; }
            for (size_t idx = 0; idx < cores_to_display.size(); ++idx) {
                int core_id = cores_to_display[idx];
                if (core_id < num_cores && !core_temp_logs[core_id].empty()) {
                    plt::named_plot("Core " + std::to_string(core_id), time_log, core_temp_logs[core_id], colors[idx % colors.size()]);
                }
            }
            plt::title("Per-Core Temperature History");
            plt::ylabel("Temperature (C)");
            plt::ylim(temp_min - 5, temp_max + 5);
            plt::legend();
            plt::grid(true);

            // 子图4: 总体温度
            plt::subplot2grid(4, 1, 3, 0);
            if (!temp_logs.empty()) {
                plt::named_plot("CPU Temp", time_log, temp_logs, "r");
            }
            plt::title("Overall CPU Temperature History");
            plt::ylabel("Temperature (C)");
            plt::xlabel("Time (s)");
            plt::grid(true);

            plt::suptitle("CPU Stress Test Results");
            plt::tight_layout();
            plt::save("cpu_stress_result.png");
            plt::close();

            std::cout << "Plot saved to cpu_stress_result.png\n";
        } catch (const std::exception& e) {
            std::cerr << "Error generating plot: " << e.what() << "\n";
        }
    }
#else
    if (enable_plot) {
        std::cout << "\nNote: matplotlibcpp is not available. Plot generation is disabled.\n";
    }
#endif

    // --- 恢复 CPU 频率设置 ---
    if (freq_capped) {
        bool ok = true;
        for (int i = 0; i < num_cores; ++i) {
            std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
            // 先恢复 max, 再恢复 min, 避免约束冲突
            if (!original_max_freqs[i].empty())
                if (!write_sysfs(base + "scaling_max_freq", original_max_freqs[i])) ok = false;
            if (!original_min_freqs[i].empty())
                if (!write_sysfs(base + "scaling_min_freq", original_min_freqs[i])) ok = false;
        }
        if (ok) std::cout << "scaling_min/max_freq restored\n";
    }
    if (governor_changed && !original_governor.empty()) {
        bool ok = true;
        for (int i = 0; i < num_cores; ++i) {
            if (!write_sysfs("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor", original_governor))
                ok = false;
        }
        if (ok) std::cout << "Governor restored to '" << original_governor << "'\n";
    }

    std::cout << "\nDone!\n";

    return 0;
}