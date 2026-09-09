/**
 * @file    main.cpp
 * @brief   NovAtel OEM7 GNSS BIN解析工具命令行入口
 * @details 纯C++17实现，跨Windows/Linux/macOS。
 *          支持单文件解析、指定输出目录、批量解析文件夹。
 *          解析NovAtel OEM7接收机输出的BIN格式文件。
 *
 * 用法示例：
 *   gnss_parser -i data.bin                          # 单文件解析，输出到./output
 *   gnss_parser -i data.bin -o ./result              # 指定输出目录
 *   gnss_parser -d ./bin_folder -o ./result          # 批量解析文件夹内所有.bin
 *   gnss_parser -i data.bin -p myprefix              # 自定义输出文件前缀
 *   gnss_parser --help                               # 显示帮助
 */

#include "bin_io/bin_reader.h"
#include "parser/bin_parser.h"
#include "export/csv_exporter.h"
#include "gnss_struct/gnss_time.h"
#include "gnss_struct/range_obs.h"
#include "gnss_struct/sat_vis.h"
#include "gnss_struct/best_pos.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define F_OK 0
    #define access _access
#else
    #include <unistd.h>
#endif

/**
 * @brief 命令行参数配置
 */
struct CliConfig {
    std::string input_file;
    std::string input_dir;
    std::string output_dir = "./output";
    std::string file_prefix = "gnss";
    bool show_help = false;
    bool verbose = false;
};

/**
 * @brief 打印工具帮助信息
 */
void print_help(const char* program_name) {
    std::cout << "NovAtel OEM7 GNSS BIN解析工具 v1.0.0\n"
              << "用法: " << program_name << " [选项]\n\n"
              << "选项:\n"
              << "  -i <file>       指定单个BIN文件路径\n"
              << "  -d <dir>        指定批量输入目录（解析目录内所有*.bin文件）\n"
              << "  -o <dir>        指定输出目录（默认: ./output）\n"
              << "  -p <prefix>     输出CSV文件名前缀（默认: gnss）\n"
              << "  -v              详细输出模式，打印每帧摘要\n"
              << "  --help          显示此帮助信息\n\n"
              << "示例:\n"
              << "  " << program_name << " -i 20250701_gps.bin\n"
              << "  " << program_name << " -i data.bin -o ./result\n"
              << "  " << program_name << " -d ./bin_folder -o ./csv_output -v\n"
              << "  " << program_name << " -i data.bin -p beidou_test\n\n"
              << "输出文件:\n"
              << "  {prefix}_range.csv    RANGE观测数据（伪距/载波相位/多普勒/CN0）\n"
              << "  {prefix}_satvis.csv   SATVIS卫星可见性（仰角/方位角）\n"
              << "  {prefix}_satvis2.csv  SATVIS2卫星可见性扩展（仰角/方位角/健康度）\n"
              << "  {prefix}_bestpos.csv  BESTPOSA定位结果（纬度/经度/大地高/精度）\n\n"
              << "NovAtel OEM7 BIN帧格式说明:\n"
              << "  同步头: 0xAA 0x44 0x12 (NovAtel标准)\n"
              << "  帧头长度: 28字节（OEM7标准）\n"
              << "  MsgID 42 = BESTPOS定位结果帧\n"
              << "  MsgID 43 = RANGE观测帧\n"
              << "  MsgID 48 = SATVIS卫星可见性帧\n"
              << "  所有数值字段均为大端字节序存储\n"
              << std::endl;
}

/**
 * @brief 解析命令行参数
 */
bool parse_cli_args(int argc, char* argv[], CliConfig& config) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--help") {
            config.show_help = true;
            return true;
        }
        else if (arg == "-i" && i + 1 < argc) {
            config.input_file = argv[++i];
        }
        else if (arg == "-d" && i + 1 < argc) {
            config.input_dir = argv[++i];
        }
        else if (arg == "-o" && i + 1 < argc) {
            config.output_dir = argv[++i];
        }
        else if (arg == "-p" && i + 1 < argc) {
            config.file_prefix = argv[++i];
        }
        else if (arg == "-v") {
            config.verbose = true;
        }
        else {
            std::cerr << "错误: 未知参数 '" << arg << "'。使用 --help 查看帮助。" << std::endl;
            return false;
        }
    }

    return true;
}

/**
 * @brief 获取目录下所有.bin文件
 */
std::vector<std::string> list_bin_files(const std::string& dir_path) {
    std::vector<std::string> files;

#ifdef _WIN32
    std::string pattern = dir_path + "\\*.bin";
    WIN32_FIND_DATAW find_data;
    std::wstring wpattern(pattern.begin(), pattern.end());
    HANDLE h_find = FindFirstFileW(wpattern.c_str(), &find_data);

    if (h_find != INVALID_HANDLE_VALUE) {
        do {
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring wname(find_data.cFileName);
                files.push_back(std::string(wname.begin(), wname.end()));
            }
        } while (FindNextFileW(h_find, &find_data));
        FindClose(h_find);
    }
#else
    std::string cmd = "ls \"" + dir_path + "\"/*.bin 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            size_t last_slash = line.find_last_of('/');
            if (last_slash != std::string::npos) {
                line = line.substr(last_slash + 1);
            }
            if (!line.empty()) {
                files.push_back(line);
            }
        }
        pclose(pipe);
    }
#endif

    std::sort(files.begin(), files.end());

    return files;
}

/**
 * @brief 解析单个BIN文件
 */
bool parse_single_file(const std::string& filepath, const CliConfig& config) {
    std::string filename = filepath;
    {
        size_t last_slash = filename.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            filename = filename.substr(last_slash + 1);
        }
        size_t dot_pos = filename.find_last_of('.');
        if (dot_pos != std::string::npos) {
            filename = filename.substr(0, dot_pos);
        }
    }

    std::string prefix = config.file_prefix + "_" + filename;

    std::cout << "解析文件: " << filepath << std::endl;
    std::cout << "输出目录: " << config.output_dir << std::endl;
    std::cout << "输出前缀: " << prefix << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    gnss::bin_io::MmapFile mmap_file(filepath);
    std::cout << "文件大小: " << mmap_file.file_size() << " 字节 ("
              << std::fixed << std::setprecision(2)
              << (mmap_file.file_size() / 1024.0 / 1024.0) << " MB)" << std::endl;

    if (mmap_file.file_size() == 0) {
        std::cerr << "警告: 文件为空，无数据可解析。" << std::endl;
        return false;
    }

    gnss::export_csv::CsvExporter exporter(config.output_dir, prefix);

    gnss::parser::BinParser parser;

    parser.set_on_range([&](const gnss::RangeFrame& frame) {
        exporter.write_range_frame(frame);
        if (config.verbose && frame.is_valid()) {
            std::cout << "  " << frame.summary() << std::endl;
        }
    });

    parser.set_on_satvis([&](const gnss::SatVisFrame& frame) {
        exporter.write_satvis_frame(frame);
        if (config.verbose && frame.is_valid()) {
            std::cout << "  " << frame.summary() << std::endl;
        }
    });

    parser.set_on_satvis2([&](const gnss::SatVis2Frame& frame) {
        exporter.write_satvis2_frame(frame);
        if (config.verbose && frame.is_valid()) {
            std::cout << "  " << frame.summary() << std::endl;
        }
    });

    parser.set_on_bestpos([&](const gnss::BestPosFrame& frame) {
        exporter.write_bestpos_frame(frame);
        if (config.verbose && frame.is_valid()) {
            std::cout << "  " << frame.summary() << std::endl;
        }
    });

    size_t last_progress_pct = 0;
    parser.set_on_progress([&](size_t processed, size_t total) {
        if (total == 0) return;
        size_t pct = processed * 100 / total;
        if (pct != last_progress_pct && pct % 10 == 0) {
            std::cout << "  进度: " << pct << "% ("
                      << (processed / 1024.0 / 1024.0) << " MB / "
                      << (total / 1024.0 / 1024.0) << " MB)" << std::endl;
            last_progress_pct = pct;
        }
    });

    parser.set_on_log([](const std::string& msg) {
        std::cerr << "  [解析] " << msg << std::endl;
    });

    std::cout << "开始解析..." << std::endl;
    auto stats = parser.parse(mmap_file);

    exporter.flush_and_close();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    auto csv_stats = exporter.stats();

    std::cout << "\n========== 解析完成 ==========" << std::endl;
    std::cout << "总耗时: " << elapsed_ms << " ms" << std::endl;
    std::cout << "处理字节: " << stats.bytes_processed << " ("
              << std::fixed << std::setprecision(2)
              << (stats.bytes_processed / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "成功解析帧数: " << stats.total_frames << std::endl;
    std::cout << "  - RANGE观测帧:     " << stats.range_frames << std::endl;
    std::cout << "  - SATVIS可见性帧:  " << stats.satvis_frames << std::endl;
    std::cout << "  - SATVIS2可见性帧: " << stats.satvis2_frames << std::endl;
    std::cout << "  - BESTPOS定位帧:   " << stats.bestpos_frames << std::endl;
    std::cout << "同步丢失: " << stats.sync_lost_count << std::endl;
    std::cout << "CRC校验失败: " << stats.crc_error_count << std::endl;
    std::cout << "\nCSV导出行数:" << std::endl;
    std::cout << "  - RANGE行:     " << csv_stats.range_rows << std::endl;
    std::cout << "  - SATVIS行:   " << csv_stats.satvis_rows << std::endl;
    std::cout << "  - SATVIS2行:  " << csv_stats.satvis2_rows << std::endl;
    std::cout << "  - BESTPOS行:  " << csv_stats.bestpos_rows << std::endl;

    std::cout << "\n输出文件:" << std::endl;
    std::cout << "  " << config.output_dir << "/" << prefix << "_range.csv" << std::endl;
    std::cout << "  " << config.output_dir << "/" << prefix << "_satvis.csv" << std::endl;
    std::cout << "  " << config.output_dir << "/" << prefix << "_satvis2.csv" << std::endl;
    std::cout << "  " << config.output_dir << "/" << prefix << "_bestpos.csv" << std::endl;

    return stats.total_frames > 0;
}

/**
 * @brief 程序主入口
 */
int main(int argc, char* argv[]) {
    CliConfig config;
    if (!parse_cli_args(argc, argv, config)) {
        return 1;
    }

    if (config.show_help) {
        print_help(argv[0]);
        return 0;
    }

    if (config.input_file.empty() && config.input_dir.empty()) {
        std::cerr << "错误: 必须指定输入文件(-i)或输入目录(-d)。\n"
                  << "使用 --help 查看帮助。" << std::endl;
        return 1;
    }

    if (!config.input_file.empty() && !config.input_dir.empty()) {
        std::cerr << "错误: -i 和 -d 不能同时使用。" << std::endl;
        return 1;
    }

    if (!config.input_file.empty()) {
        if (access(config.input_file.c_str(), F_OK) != 0) {
            std::cerr << "错误: 文件不存在: " << config.input_file << std::endl;
            return 1;
        }

        if (!parse_single_file(config.input_file, config)) {
            std::cerr << "解析失败或未找到有效帧。" << std::endl;
            return 1;
        }
        return 0;
    }

    if (!config.input_dir.empty()) {
        if (access(config.input_dir.c_str(), F_OK) != 0) {
            std::cerr << "错误: 目录不存在: " << config.input_dir << std::endl;
            return 1;
        }

        auto bin_files = list_bin_files(config.input_dir);
        if (bin_files.empty()) {
            std::cerr << "错误: 目录中没有找到.bin文件: " << config.input_dir << std::endl;
            return 1;
        }

        std::cout << "批量解析模式: 找到 " << bin_files.size() << " 个BIN文件\n" << std::endl;

        int success_count = 0;
        int fail_count = 0;

        for (size_t i = 0; i < bin_files.size(); ++i) {
            std::string full_path = config.input_dir + "/" + bin_files[i];
            std::cout << "[" << (i + 1) << "/" << bin_files.size() << "] "
                      << bin_files[i] << std::endl;

            try {
                if (parse_single_file(full_path, config)) {
                    ++success_count;
                } else {
                    ++fail_count;
                }
            } catch (const std::exception& e) {
                std::cerr << "  异常: " << e.what() << std::endl;
                ++fail_count;
            }

            std::cout << std::endl;
        }

        std::cout << "========== 批量解析完成 ==========" << std::endl;
        std::cout << "成功: " << success_count << " 文件" << std::endl;
        std::cout << "失败: " << fail_count << " 文件" << std::endl;

        return fail_count > 0 ? 1 : 0;
    }

    return 0;
}
