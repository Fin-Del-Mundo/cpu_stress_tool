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
static int g_power_percent = 100;  // 全局功率百分比 (0-100), 100 表示跑满

void signal_handler(int signal) {
    g_stop = 1;
}

// --- 压力测试模块 ---
void stress_worker(int core_id, int total_threads, int thread_index) {
    // 将当前线程绑定到指定的 CPU 核心上，确保只有所需的核心跑满载
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t thread = pthread_self();
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    // 如果功率百分比为 100，则跑满
    if (g_power_percent >= 100) {
        while (g_stop == 0) {
            double x = 0.1;
            for (int i = 0; i < 5000; ++i) {
                x = std::sin(x) * std::cos(x) + std::tan(x);
            }
        }
    } else {
        // 使用更精细的负载控制方式
        // 原理：在小的周期内精确控制工作和休眠时间比例
        // 同时错开各线程的周期，避免同步导致的负载波动

        const int cycle_us = 1000;  // 1ms 周期
        int work_us = (g_power_percent * cycle_us) / 100;
        int sleep_us = cycle_us - work_us;

        // 计算线程的初始偏移量，使各线程的工作周期错开
        int offset_us = (thread_index * cycle_us) / total_threads;
        std::this_thread::sleep_for(std::chrono::microseconds(offset_us));

        while (g_stop == 0) {
            auto start = std::chrono::steady_clock::now();

            // 工作阶段
            auto work_end_time = start + std::chrono::microseconds(work_us);
            while (std::chrono::steady_clock::now() < work_end_time && g_stop == 0) {
                double x = 0.1;
                for (int i = 0; i < 1000; ++i) {
                    x = std::sin(x) * std::cos(x) + std::tan(x);
                }
            }

            // 休眠阶段
            if (sleep_us > 0 && g_stop == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
            }
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
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(core_id) + "/cpufreq/scaling_cur_freq";
    std::ifstream file(path);
    double freq_khz = 0;
    if (file.is_open()) file >> freq_khz;
    return freq_khz / 1000.0;
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

// 通过 coretemp 驱动获取各核心温度
// 返回: map<核心标签, 温度值>，如 {"Core 0": 45.0, "Package id 0": 50.0}
std::map<std::string, double> get_core_temperatures() {
    std::map<std::string, double> core_temps;
    namespace fs = std::filesystem;
    std::string hwmon_base = "/sys/class/hwmon";

    try {
        if (fs::exists(hwmon_base)) {
            for (const auto& hwmon_entry : fs::directory_iterator(hwmon_base)) {
                if (!hwmon_entry.is_directory()) continue;

                std::string hwmon_dir = hwmon_entry.path().string();
                std::string name_file = hwmon_dir + "/name";

                // 检查驱动名称是否为 coretemp
                if (fs::exists(name_file)) {
                    std::ifstream name_stream(name_file);
                    std::string driver_name;

                    if (std::getline(name_stream, driver_name)) {
                        // 去除末尾空白
                        driver_name.erase(driver_name.find_last_not_of(" \n\r\t") + 1);

                        if (driver_name == "coretemp") {
                            // 遍历目录下的所有 temp*_label 文件
                            for (const auto& temp_entry : fs::directory_iterator(hwmon_dir)) {
                                std::string entry_name = temp_entry.path().filename().string();

                                // 查找 temp*_label 文件
                                if (entry_name.find("temp") == 0 &&
                                    entry_name.find("_label") != std::string::npos) {

                                    std::string label_file = temp_entry.path().string();
                                    std::ifstream label_stream(label_file);
                                    std::string label;

                                    if (std::getline(label_stream, label)) {
                                        label.erase(label.find_last_not_of(" \n\r\t") + 1);

                                        // 构造对应的 _input 文件名
                                        std::string input_file = label_file;
                                        size_t label_pos = input_file.find("_label");
                                        if (label_pos != std::string::npos) {
                                            input_file.replace(label_pos, 6, "_input");

                                            if (fs::exists(input_file)) {
                                                std::ifstream input_stream(input_file);
                                                int64_t raw_temp = 0;

                                                if (input_stream >> raw_temp) {
                                                    double temp_c = raw_temp / 1000.0;
                                                    core_temps[label] = temp_c;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
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
        "cpu",            // 通用 CPU 标识
        "coretemp",       // coretemp 驱动
        "cpu_thermal",    // ARM CPU 热传感器
        "soc_thermal",    // SoC 热传感器
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

// 获取 CPU 平均温度 (单位: 摄氏度)
double get_cpu_temp() {
    std::vector<double> valid_temps;

    // 优先从 coretemp 驱动读取核心温度
    auto core_temps = get_core_temperatures();

    if (!core_temps.empty()) {
        for (const auto& [label, temp] : core_temps) {
            if (label.find("Core") != std::string::npos ||
                label.find("Package") != std::string::npos) {
                if (temp > 0 && temp < 150) {
                    valid_temps.push_back(temp);
                }
            }
        }
    }

    // 如果 coretemp 未找到，尝试 thermal_zone
    if (valid_temps.empty()) {
        double tz_temp = get_cpu_temp_from_thermal_zone();
        if (tz_temp > 0) {
            valid_temps.push_back(tz_temp);
        }
    }

    // 计算平均温度
    if (!valid_temps.empty()) {
        double sum = std::accumulate(valid_temps.begin(), valid_temps.end(), 0.0);
        double avg_temp = sum / valid_temps.size();

        // 应用温度平滑
        return g_temp_smoother.smooth(avg_temp);
    }

    return 0.0;
}

// 获取单个核心的温度 (单位: 摄氏度)
double get_core_temp(int core_id) {
    auto core_temps = get_core_temperatures();

    // 尝试匹配 "Core N" 标签
    std::string target_label = "Core " + std::to_string(core_id);
    if (core_temps.find(target_label) != core_temps.end()) {
        return core_temps[target_label];
    }

    // 备用: 如果找不到特定核心温度，返回 Package 温度
    for (const auto& [label, temp] : core_temps) {
        if (label.find("Package") != std::string::npos) {
            return temp;
        }
    }

    return 0.0;
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
            std::string power_str = arg.substr(8);  // 提取 "--power=" 后的内容
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
                  << "\nExamples:\n"
                  << "  " << argv[0] << "                    # Use all cores at 100% power\n"
                  << "  " << argv[0] << " --power=50           # Use all cores at 50% power\n"
                  << "  " << argv[0] << " --cores=0-3 --power=75  # Use cores 0-3 at 75% power\n";
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

    // --- 设置 CPU Governor 为 performance 模式 ---
    // 这是解决频率跳变的关键：powersave 模式会根据负载动态调整频率
    // performance 模式会锁定频率在最大值，保证稳定
    std::string original_governor;
    bool governor_changed = false;

    {
        // 读取当前 governor（从 cpu0）
        std::ifstream gov_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        if (gov_file.is_open()) {
            std::getline(gov_file, original_governor);
            // 去除空白
            original_governor.erase(original_governor.find_last_not_of(" \n\r\t") + 1);
        }

        if (original_governor != "performance") {
            // 尝试为所有核心设置 performance governor
            bool all_success = true;
            for (int i = 0; i < num_cores; ++i) {
                std::string gov_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
                std::ofstream set_gov(gov_path);
                if (set_gov.is_open()) {
                    set_gov << "performance";
                    set_gov.close();
                } else {
                    all_success = false;
                }
            }

            if (all_success) {
                // 验证是否设置成功
                bool verified = true;
                for (int i = 0; i < num_cores; ++i) {
                    std::string gov_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
                    std::ifstream check_gov(gov_path);
                    std::string new_gov;
                    if (check_gov.is_open() && std::getline(check_gov, new_gov)) {
                        new_gov.erase(new_gov.find_last_not_of(" \n\r\t") + 1);
                        if (new_gov != "performance") {
                            verified = false;
                            break;
                        }
                    } else {
                        verified = false;
                        break;
                    }
                }

                if (verified) {
                    governor_changed = true;
                    std::cout << "CPU Governor set to 'performance' mode for all " << num_cores << " cores (was: " << original_governor << ")\n";
                }
            }

            if (!governor_changed) {
                std::cout << "Warning: Could not set CPU governor to 'performance'.\n";
                std::cout << "         CPU frequency may fluctuate. Run with 'sudo' to enable performance mode.\n";
                std::cout << "         Or manually run: sudo cpupower frequency-set -g performance\n";
            }
        }
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
    } else {
        std::cout << "Power mode: " << g_power_percent << "% power\n";
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

    // --- 恢复 CPU Governor 设置 ---
    if (governor_changed && !original_governor.empty()) {
        bool restore_success = true;
        for (int i = 0; i < num_cores; ++i) {
            std::string gov_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
            std::ofstream set_gov(gov_path);
            if (set_gov.is_open()) {
                set_gov << original_governor;
            } else {
                restore_success = false;
            }
        }
        if (restore_success) {
            std::cout << "CPU Governor restored to '" << original_governor << "' for all " << num_cores << " cores\n";
        }
    }

    std::cout << "\nDone!\n";

    return 0;
}