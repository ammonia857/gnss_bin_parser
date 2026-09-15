/**
 * @file    csv_exporter.cpp
 * @brief   CSV导出器实现
 * @details 将GNSS结构化数据导出为UTF-8（带BOM）CSV文件。
 *          支持四类输出：RANGE观测、SATVIS卫星可见性、SATVIS2扩展可见性、
 *          BESTPOS定位结果。每条观测/卫星/定位结果占一行，列间以逗号分隔。
 *
 *          文件采用**惰性创建**：某类数据第一次写入时才创建文件并写表头；
 *          全程无数据则不会创建任何CSV文件。
 *
 *          CSV列定义（追加列一律在行尾，既有列顺序不变）：
 *          - RANGE:    GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,
 *                      PsrStd_m,CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,
 *                      CN0_dBHz,LockTime_s,ChTrStatus,Sat_System_Name
 *          - SATVIS:   GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,
 *                      Azimuth_deg,Sat_System_Name
 *          - SATVIS2:  GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,
 *                      Azimuth_deg,Health,GloFreq,TrueDoppler_Hz,
 *                      ApparentDoppler_Hz,Sat_System_Name
 *          - BESTPOS:  GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,
 *                      Longitude_deg,Height_m(MSL),Undulation_m,LatStd_m,LonStd_m,
 *                      HgtStd_m,SVs_Tracked,SVs_Used
 *
 *          说明：Sat_System 是项目内部枚举（0=GPS,1=BDS,2=GLONASS,3=GALILEO,
 *          4=SBAS,5=QZSS,6=NAVIC,7=OTHER,255=UNKNOWN）；GloFreq 语义为
 *          GLONASS Frequency + 7；Height_m 为 MSL 高程（椭球高 = Height_m +
 *          Undulation_m）。
 */

#include "export/csv_exporter.h"

#include <sstream>
#include <iomanip>
#include <cstdio>
#include <iostream>
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

namespace {

// ============================================================
// 表头（与写入顺序严格一致）
// ============================================================

constexpr const char* RANGE_HEADER =
    "GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,"
    "CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s,"
    "ChTrStatus,Sat_System_Name";

constexpr const char* SATVIS_HEADER =
    "GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,"
    "Sat_System_Name";

constexpr const char* SATVIS2_HEADER =
    "GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,Health,"
    "GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name";

// 注意写入顺序：先 #SVs(tracked) 再 #solnSVs(used)
constexpr const char* BESTPOS_HEADER =
    "GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,Longitude_deg,"
    "Height_m,Undulation_m,LatStd_m,LonStd_m,HgtStd_m,SVs_Tracked,SVs_Used";

/**
 * @brief 将 ch-tr-status 格式化为十六进制字符串（如 "0x08109C04"）
 * @note  用 snprintf 生成，避免 hex/setw/setfill 等流标志粘连影响后续列
 */
std::string format_hex32(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(value));
    return std::string(buf);
}

} // namespace

// ============================================================
// 构造函数 & 析构
// ============================================================

CsvExporter::CsvExporter(const std::string& output_dir,
                         const std::string& file_prefix)
    : output_dir_(output_dir)
    , file_prefix_(file_prefix)
{
    ensure_directory(output_dir_);

    // 仅构建路径；文件在**首次写入该类数据时**才创建（惰性建文件）
    range_filepath_    = output_dir_ + "/" + file_prefix_ + "_range.csv";
    satvis_filepath_   = output_dir_ + "/" + file_prefix_ + "_satvis.csv";
    satvis2_filepath_  = output_dir_ + "/" + file_prefix_ + "_satvis2.csv";
    bestpos_filepath_  = output_dir_ + "/" + file_prefix_ + "_bestpos.csv";
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
// CSV文件惰性打开
// ============================================================

bool CsvExporter::ensure_csv_open(std::ofstream& file_stream,
                                  const std::string& filepath,
                                  const char* header_line) {
    if (closed_) return false;
    if (file_stream.is_open()) return true;

    file_stream.open(filepath, std::ios::out | std::ios::trunc);
    if (!file_stream.is_open()) {
        ++write_error_count_;
        if (!open_error_reported_) {
            open_error_reported_ = true;
            std::cerr << "[csv_exporter] 无法创建CSV文件: " << filepath << std::endl;
        }
        return false;
    }

    // 写入UTF-8 BOM（确保Excel正确识别中文）
    // BOM: EF BB BF
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    file_stream.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    file_stream << header_line << "\n";

    if (!file_stream) {
        ++write_error_count_;
        return false;
    }
    return true;
}

void CsvExporter::note_stream_state(const std::ofstream& file_stream) noexcept {
    // fail() 覆盖 failbit|badbit：写盘失败必须可见，不能静默丢数据
    if (file_stream.fail()) {
        ++write_error_count_;
    }
}

void CsvExporter::close_stream(std::ofstream& file_stream) noexcept {
    if (!file_stream.is_open()) return;

    file_stream.flush();
    if (file_stream.fail()) {
        ++write_error_count_;
    }
    file_stream.close();
    if (file_stream.fail()) {
        ++write_error_count_;
    }
}

// ============================================================
// RANGE观测写入
// ============================================================

void CsvExporter::write_range_frame(const RangeFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!ensure_csv_open(range_file_, range_filepath_, RANGE_HEADER)) return;

    // 每条观测展开为一行
    for (const auto& obs : frame.observations) {
        range_file_ << frame.gps_time.week << ","
                    << frame.gps_time.tow_ms << ","
                    << static_cast<int>(obs.sat_system) << ","
                    << obs.sat_prn << ","
                    << static_cast<int>(obs.glofreq) << ","
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
                    << static_cast<double>(obs.cn0) << ","
                    << std::setprecision(1)
                    << static_cast<double>(obs.locktime) << ","
                    << format_hex32(obs.ch_tr_status) << ","
                    << system_to_name(obs.sat_system) << "\n";
        ++export_stats_.range_rows;
    }

    note_stream_state(range_file_);

    // 每100行刷新一次缓冲区
    if (export_stats_.range_rows % 100 == 0) {
        range_file_.flush();
        note_stream_state(range_file_);
    }
}

// ============================================================
// SATVIS卫星可见性写入
// ============================================================

void CsvExporter::write_satvis_frame(const SatVisFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!ensure_csv_open(satvis_file_, satvis_filepath_, SATVIS_HEADER)) return;

    // 每颗卫星展开为一行
    for (const auto& sat : frame.sats) {
        satvis_file_ << frame.gps_time.week << ","
                     << frame.gps_time.tow_ms << ","
                     << static_cast<int>(frame.sat_system) << ","
                     << static_cast<int>(sat.sat_prn) << ","
                     << std::fixed << std::setprecision(1)
                     << static_cast<double>(sat.elevation) << ","
                     << static_cast<double>(sat.azimuth) << ","
                     << system_to_name(frame.sat_system) << "\n";
        ++export_stats_.satvis_rows;
    }

    note_stream_state(satvis_file_);

    if (export_stats_.satvis_rows % 100 == 0) {
        satvis_file_.flush();
        note_stream_state(satvis_file_);
    }
}

// ============================================================
// BESTPOS定位结果写入
// ============================================================

void CsvExporter::write_bestpos_frame(const BestPosFrame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!ensure_csv_open(bestpos_file_, bestpos_filepath_, BESTPOS_HEADER)) return;

    // 列顺序：...,SVs_Tracked(#SVs),SVs_Used(#solnSVs) —— 与表头一致
    bestpos_file_ << frame.gps_time.week << ","
                  << frame.gps_time.tow_ms << ","
                  << static_cast<uint32_t>(frame.solution_status) << ","
                  << frame.position_type << ","
                  << std::fixed << std::setprecision(9)
                  << frame.latitude << ","
                  << frame.longitude << ","
                  << std::setprecision(4)
                  << frame.height << ","
                  << frame.undulation << ","
                  << frame.lat_std_dev << ","
                  << frame.lon_std_dev << ","
                  << frame.hgt_std_dev << ","
                  << static_cast<unsigned>(frame.num_svs) << ","
                  << static_cast<unsigned>(frame.num_soln_svs) << "\n";
    ++export_stats_.bestpos_rows;

    note_stream_state(bestpos_file_);

    if (export_stats_.bestpos_rows % 100 == 0) {
        bestpos_file_.flush();
        note_stream_state(bestpos_file_);
    }
}

// ============================================================
// SATVIS2卫星可见性写入
// ============================================================

void CsvExporter::write_satvis2_frame(const SatVis2Frame& frame) {
    if (!frame.is_valid()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!ensure_csv_open(satvis2_file_, satvis2_filepath_, SATVIS2_HEADER)) return;

    for (const auto& sat : frame.sats) {
        satvis2_file_ << frame.gps_time.week << ","
                      << frame.gps_time.tow_ms << ","
                      << static_cast<int>(frame.sat_system) << ","
                      << sat.sat_prn << ","
                      << std::fixed << std::setprecision(1)
                      << sat.elevation << ","
                      << sat.azimuth << ","
                      // health 是 uint32，显式以无符号整数输出，
                      // 避免受 std::fixed/setprecision 影响而带上小数点
                      << static_cast<uint32_t>(sat.health) << ","
                      << static_cast<int>(sat.glofreq) << ","
                      << std::setprecision(3)
                      << sat.true_doppler << ","
                      << sat.apparent_doppler << ","
                      << system_to_name(frame.sat_system) << "\n";
        ++export_stats_.satvis2_rows;
    }

    note_stream_state(satvis2_file_);

    if (export_stats_.satvis2_rows % 100 == 0) {
        satvis2_file_.flush();
        note_stream_state(satvis2_file_);
    }
}

// ============================================================
// 关闭与清理
// ============================================================

void CsvExporter::flush_and_close() {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (closed_) return;
    closed_ = true;

    close_stream(range_file_);
    close_stream(satvis_file_);
    close_stream(satvis2_file_);
    close_stream(bestpos_file_);
}

bool CsvExporter::had_write_error() const noexcept {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return write_error_count_ > 0;
}

size_t CsvExporter::write_error_count() const noexcept {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return write_error_count_;
}

std::string CsvExporter::format_time_stamp(const GpsTime& gt) {
    std::ostringstream oss;
    oss << "W" << gt.week << "_T" << gt.tow_ms;
    return oss.str();
}

} // namespace export_csv
} // namespace gnss
