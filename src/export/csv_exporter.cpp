/**
 * @file    csv_exporter.cpp
 * @brief   CSV导出器实现
 * @details 将GNSS结构化数据按时间分段导出为UTF-8编码CSV文件。
 *          支持三种输出：RANGE观测、SATVIS卫星可见性、BESTPOS定位结果。
 *          每条观测/卫星/定位结果占一行，列间以逗号分隔。
 *          文件名包含类型和时间信息，便于按时间段检索。
 *
 *          CSV列定义：
 *          - RANGE: GPS_Week, TOW_ms, Sat_System, Sat_PRN, Freq_Band, Pseudorange_m, CarrierPhase_cycle
 *          - SATVIS: GPS_Week, TOW_ms, Sat_System, Sat_PRN, Elevation_deg, Azimuth_deg
 *          - BESTPOS: GPS_Week, TOW_ms, Solution_Status, Latitude_deg, Longitude_deg, Height_m, LatStd_m, LonStd_m, HgtStd_m
 */

#include "export/csv_exporter.h"

#include <sstream>
#include <iomanip>
#include <ctime>

#ifdef _WIN32
    #include <direct.h>
    #define mkdir_func(path) _mkdir(path)
#else
    #include <sys/stat.h>
    #define mkdir_func(path) mkdir(path, 0755)
#endif

namespace gnss {
namespace export_csv {

// ============================================================
// 构造函数 & 析构
// ============================================================

CsvExporter::CsvExporter(const std::string& output_dir,
                         const std::string& file_prefix)
    : output_dir_(output_dir)
    , file_prefix_(file_prefix)
{
    ensure_directory(output_dir_);

    // 构建文件路径
    range_filepath_    = output_dir_ + "/" + file_prefix_ + "_range.csv";
    satvis_filepath_   = output_dir_ + "/" + file_prefix_ + "_satvis.csv";
    satvis2_filepath_  = output_dir_ + "/" + file_prefix_ + "_satvis2.csv";
    bestpos_filepath_  = output_dir_ + "/" + file_prefix_ + "_bestpos.csv";

    // 打开CSV文件并写入表头
    open_csv(range_file_, range_filepath_,
        "GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s");

    open_csv(satvis_file_, satvis_filepath_,
        "GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg");

    open_csv(satvis2_file_, satvis2_filepath_,
        "GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,Health");

    open_csv(bestpos_file_, bestpos_filepath_,
        "GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,Longitude_deg,Height_m,Undulation_m,LatStd_m,LonStd_m,HgtStd_m,SVs_Used,SVs_Tracked");
}

CsvExporter::~CsvExporter() {
    flush_and_close();
}

// ============================================================
// 目录创建
// ============================================================

void CsvExporter::ensure_directory(const std::string& dir) {
    if (dir.empty()) return;

    // 尝试创建目录（如果已存在则忽略错误）
    mkdir_func(dir.c_str());
}

// ============================================================
// CSV文件打开
// ============================================================

void CsvExporter::open_csv(std::ofstream& file_stream,
                            const std::string& filepath,
                            const std::string& header_line) {
    file_stream.open(filepath, std::ios::out | std::ios::trunc);
    if (!file_stream.is_open()) {
        throw std::runtime_error("无法创建CSV文件: " + filepath);
    }

    // 写入UTF-8 BOM（确保Excel正确识别中文）
    // BOM: EF BB BF
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    file_stream.write(reinterpret_cast<const char*>(bom), sizeof(bom));

    // 写入表头
    file_stream << header_line << "\n";
    file_stream.flush();
}

// ============================================================
// RANGE观测写入
// ============================================================

void CsvExporter::write_range_frame(const RangeFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!range_file_.is_open()) return;

    // 每条观测展开为一行
    for (const auto& obs : frame.observations) {
        range_file_ << frame.gps_time.week << ","
                    << frame.gps_time.tow_ms << ","
                    << static_cast<int>(obs.sat_system) << ","
                    << obs.sat_prn << ","
                    << obs.glofreq << ","
                    << std::fixed << std::setprecision(3)
                    << obs.pseudorange << ","
                    << std::setprecision(4)
                    << obs.psr_std << ","
                    << std::setprecision(3)
                    << obs.carrier_phase << ","
                    << std::setprecision(4)
                    << obs.adr_std << ","
                    << std::setprecision(3)
                    << obs.doppler << ","
                    << std::setprecision(2)
                    << obs.cn0 << ","
                    << std::setprecision(1)
                    << obs.locktime << "\n";
        ++export_stats_.range_rows;
    }

    // 每100行刷新一次缓冲区
    if (export_stats_.range_rows % 100 == 0) {
        range_file_.flush();
    }
}

// ============================================================
// SATVIS卫星可见性写入
// ============================================================

void CsvExporter::write_satvis_frame(const SatVisFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!satvis_file_.is_open()) return;

    // 每颗卫星展开为一行
    for (const auto& sat : frame.sats) {
        satvis_file_ << frame.gps_time.week << ","
                     << frame.gps_time.tow_ms << ","
                     << static_cast<int>(frame.sat_system) << ","
                     << static_cast<int>(sat.sat_prn) << ","
                     << std::fixed << std::setprecision(1)
                     << sat.elevation << ","
                     << sat.azimuth << "\n";
        ++export_stats_.satvis_rows;
    }

    if (export_stats_.satvis_rows % 100 == 0) {
        satvis_file_.flush();
    }
}

// ============================================================
// BESTPOS定位结果写入
// ============================================================

void CsvExporter::write_bestpos_frame(const BestPosFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!bestpos_file_.is_open()) return;

    bestpos_file_ << frame.gps_time.week << ","
                  << frame.gps_time.tow_ms << ","
                  << static_cast<uint32_t>(frame.solution_status) << ","
                  << frame.position_type << ","
                  << std::fixed << std::setprecision(9)
                  << frame.latitude << ","
                  << frame.longitude << ","
                  << std::setprecision(4)
                  << frame.height << ","
                  << std::setprecision(4)
                  << frame.undulation << ","
                  << std::setprecision(4)
                  << frame.lat_std_dev << ","
                  << frame.lon_std_dev << ","
                  << frame.hgt_std_dev << ","
                  << static_cast<int>(frame.num_svs) << ","
                  << static_cast<int>(frame.num_soln_svs) << "\n";
    ++export_stats_.bestpos_rows;

    if (export_stats_.bestpos_rows % 100 == 0) {
        bestpos_file_.flush();
    }
}

// ============================================================
// SATVIS2卫星可见性写入
// ============================================================

void CsvExporter::write_satvis2_frame(const SatVis2Frame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!satvis2_file_.is_open()) return;

    for (const auto& sat : frame.sats) {
        satvis2_file_ << frame.gps_time.week << ","
                      << frame.gps_time.tow_ms << ","
                      << static_cast<int>(frame.sat_system) << ","
                      << sat.sat_prn << ","
                      << std::fixed << std::setprecision(1)
                      << sat.elevation << ","
                      << sat.azimuth << ","
                      << std::setprecision(6)
                      << sat.health << "\n";
        ++export_stats_.satvis2_rows;
    }

    if (export_stats_.satvis2_rows % 100 == 0) {
        satvis2_file_.flush();
    }
}

// ============================================================
// 关闭与清理
// ============================================================

void CsvExporter::flush_and_close() {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (range_file_.is_open()) {
        range_file_.flush();
        range_file_.close();
    }
    if (satvis_file_.is_open()) {
        satvis_file_.flush();
        satvis_file_.close();
    }
    if (satvis2_file_.is_open()) {
        satvis2_file_.flush();
        satvis2_file_.close();
    }
    if (bestpos_file_.is_open()) {
        bestpos_file_.flush();
        bestpos_file_.close();
    }
}

std::string CsvExporter::format_time_stamp(const GpsTime& gt) {
    std::ostringstream oss;
    oss << "W" << gt.week << "_T" << gt.tow_ms;
    return oss.str();
}

} // namespace export_csv
} // namespace gnss
