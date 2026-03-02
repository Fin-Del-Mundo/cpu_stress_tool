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

volatile std::sig_atomic_t g_stop = 0;
static int g_power_percent = 100;  // 全局功率百分比 (0-100), 100 表示跑满

void signal_handler(int signal) {
    g_stop = 1;
}

// --- 压力测试模块 ---
void stress_worker(int core_id) {
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
            // 增加计算复杂度以确保流水线满载
            for (int i = 0; i < 5000; ++i) {
                x = std::sin(x) * std::cos(x) + std::tan(x);
            }
        }
    } else {
        // 按百分比功率运行：工作一段时间，然后休眠
        // 使用循环周期为 100ms，工作时间比例为 power_percent%
        int cycle_ms = 100;  // 每个完整周期 100ms
        int work_ms = (g_power_percent * cycle_ms) / 100;
        auto cycle_duration = std::chrono::milliseconds(cycle_ms);
        
        while (g_stop == 0) {
            auto cycle_start = std::chrono::steady_clock::now();
            
            // 在 work_ms 内执行计算
            auto work_end = cycle_start + std::chrono::milliseconds(work_ms);
            while (std::chrono::steady_clock::now() < work_end && g_stop == 0) {
                double x = 0.1;
                for (int i = 0; i < 5000; ++i) {
                    x = std::sin(x) * std::cos(x) + std::tan(x);
                }
            }
            
            // 剩余时间休眠
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - cycle_start);
            if (elapsed < cycle_duration) {
                auto remaining = cycle_duration - elapsed;
                std::this_thread::sleep_for(remaining);
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

// 获取 CPU 温度 (单位: 摄氏度)
// 支持多个温度源: 全局温度 + 每个核心的独立温度
double get_cpu_temp() {
    // 优先使用 hwmon 接口 - 收集所有温度传感器并求平均
    // 这样可以得到更全面的温度信息，包括各个核心的温度
    std::vector<double> collected_temps;
    for (int hwmon_idx = 0; hwmon_idx < 15; ++hwmon_idx) {
        for (int temp_idx = 1; temp_idx <= 20; ++temp_idx) {
            std::string hwmon_path = "/sys/class/hwmon/hwmon" + std::to_string(hwmon_idx) + 
                                    "/temp" + std::to_string(temp_idx) + "_input";
            std::ifstream hwmon_file(hwmon_path);
            if (hwmon_file.is_open()) {
                double temp = 0.0;
                if (hwmon_file >> temp) {
                    temp = temp / 1000.0;
                    if (temp > 0 && temp < 150) {
                        collected_temps.push_back(temp);
                    }
                }
            }
        }
    }
    
    // 如果找到了有效的温度传感器，返回平均值
    if (!collected_temps.empty()) {
        double sum = 0.0;
        for (double t : collected_temps) sum += t;
        return sum / collected_temps.size();
    }
    
    // 备用方案: /sys/class/thermal/thermal_zone0/temp
    {
        std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
        if (file.is_open()) {
            double temp = 0.0;
            if (file >> temp) {
                return temp / 1000.0;
            }
        }
    }
    
    return 0.0;
}

// 获取单个核心的温度 (单位: 摄氏度)
double get_core_temp(int core_id) {
    // 尝试直接对应核心的温度传感器
    // 对于多核系统，核心 i 的温度通常在 temp(i+2)_input
    for (int hwmon_idx = 0; hwmon_idx < 20; ++hwmon_idx) {
        std::string sensor_path = "/sys/class/hwmon/hwmon" + std::to_string(hwmon_idx) + 
                                 "/temp" + std::to_string(core_id + 2) + "_input";
        std::ifstream sensor_file(sensor_path);
        if (sensor_file.is_open()) {
            double temp = 0.0;
            if (sensor_file >> temp) {
                temp = temp / 1000.0;
                if (temp > 0 && temp < 150) {  // 合理的温度范围
                    return temp;
                }
            }
        }
    }
    
    // 如果找不到核心特定的温度，尝试通用传感器
    for (int hwmon_idx = 0; hwmon_idx < 10; ++hwmon_idx) {
        for (int temp_idx = 1; temp_idx <= 20; ++temp_idx) {
            std::string sensor_path = "/sys/class/hwmon/hwmon" + std::to_string(hwmon_idx) + 
                                     "/temp" + std::to_string(temp_idx) + "_input";
            std::ifstream sensor_file(sensor_path);
            if (sensor_file.is_open()) {
                double temp = 0.0;
                if (sensor_file >> temp) {
                    temp = temp / 1000.0;
                    if (temp > 0 && temp < 150) {  // 合理的温度范围
                        return temp;
                    }
                }
            }
        }
    }
    
    // 如果找不到核心特定的温度，返回0
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
    // 在任何其他操作之前，设置matplotlib后端
#ifdef ENABLE_MATPLOTLIB
    setenv("MPLBACKEND", "Agg", 1);
#endif
    
    // 解析命令行参数
    bool enable_plot = false;
    bool enable_csv = true;
    CoreConfig core_config;
    std::string cores_str;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--plot") enable_plot = true;
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
                  << "  --plot               Generate plots with matplotlib\n"
                  << "  --no-csv             Skip CSV export\n"
                  << "  --help, -h           Show this help message\n"
                  << "\nExamples:\n"
                  << "  " << argv[0] << "                    # Use all cores at 100% power\n"
                  << "  " << argv[0] << " --power=50           # Use all cores at 50% power\n"
                  << "  " << argv[0] << " --cores=0-3 --power=75  # Use cores 0-3 at 75% power\n";
        return 0;
    }

#ifndef ENABLE_MATPLOTLIB
    if (enable_plot) {
        std::cerr << "Warning: matplotlib support is not compiled. Skipping plot generation.\n";
        enable_plot = false;
    }
#endif
    
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
    for (int core_id : test_cores) {
        // stress_worker 会将线程绑定到 core_id
        threads.emplace_back(stress_worker, core_id);
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

    // --- Matplotlib 绘图部分 (可选) ---
#ifdef ENABLE_MATPLOTLIB
    if (enable_plot && !time_log.empty()) {
        std::cout << "Generating plots with matplotlib...\n";
        
        try {
            // 获取当前可执行文件所在目录
            char exe_path[1024];
            ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
            std::string script_dir = "/home/ubuntu/Documents/my_test/my_cpu_stress";
            
            if (len != -1) {
                exe_path[len] = '\0';
                std::string exe_str(exe_path);
                size_t last_slash = exe_str.rfind('/');
                if (last_slash != std::string::npos) {
                    // 假设脚本在源目录中
                    script_dir = exe_str.substr(0, last_slash);
                    // 返回上一级目录（从build到源目录）
                    last_slash = script_dir.rfind('/');
                    if (last_slash != std::string::npos && script_dir.substr(last_slash) == "/build") {
                        script_dir = script_dir.substr(0, last_slash);
                    }
                }
            }
            
            std::string plot_script = "python3 \"" + script_dir + "/plot_data.py\" \"cpu_stress_result.csv\" \"cpu_stress_result.png\"";
            std::cout << "Running: " << plot_script << "\n";
            int result = std::system(plot_script.c_str());
            
            if (result == 0) {
                std::cout << "Plot generated successfully.\n";
            } else {
                std::cerr << "Warning: Failed to generate matplotlib plot (exit code: " << result << ")\n";
                std::cerr << "You can manually run: python3 " << script_dir << "/plot_data.py cpu_stress_result.csv\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to generate matplotlib plot: " << e.what() << "\n";
        }
    }
#endif

    std::cout << "\nDone!\n"; 

    return 0;
}