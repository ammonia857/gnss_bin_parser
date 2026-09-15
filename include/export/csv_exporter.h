#pragma once
/**
 * @file    csv_exporter.h
 * @brief   解析结果CSV导出器
 * @details 将BIN解析出的GNSS结构化数据导出为CSV文本文件。
 *          支持四类输出（均为**惰性创建**：首次写入该类数据时才创建文件并写表头）：
 *          1. RANGE观测数据CSV：伪距/载波相位/多普勒/CN0 + ch-tr-status + 系统名
 *          2. SATVIS卫星可见性CSV：仰角/方位角 + 系统名
 *          3. SATVIS2卫星可见性扩展CSV：仰角/方位角/健康度/GLONASS频率/多普勒 + 系统名
 *          4. BESTPOS定位结果CSV：纬度/经度/MSL高程/精度/卫星数
 *
 *          全程无数据时不会创建任何CSV文件。
 *          CSV使用UTF-8编码（带BOM），兼容Excel/WPS直接打开。
 *
 *          列定义（追加列一律放在行尾，保持既有列顺序不变，避免破坏下游脚本）：
 *          - RANGE:    GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,
 *                      PsrStd_m,CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,
 *                      CN0_dBHz,LockTime_s,ChTrStatus,Sat_System_Name
 *          - SATVIS:   GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,
 *                      Azimuth_deg,Sat_System_Name
 *          - SATVIS2:  GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,
 *                      Azimuth_deg,Health,GloFreq,TrueDoppler_Hz,
 *                      ApparentDoppler_Hz,Sat_System_Name
 *          - BESTPOS:  GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,
 *                      Longitude_deg,Height_m,Undulation_m,LatStd_m,LonStd_m,
 *                      HgtStd_m,SVs_Tracked,SVs_Used
 *
 *          说明：
 *          - Sat_System 为项目**内部枚举**（0=GPS,1=BDS,2=GLONASS,3=GALILEO,4=SBAS,
 *            5=QZSS,6=NAVIC,7=OTHER,255=UNKNOWN），不是官方任一编号。
 *          - GloFreq 语义为 GLONASS Frequency + 7（非GLONASS为0）。
 *          - Height_m 为 MSL 高程，椭球高 = Height_m + Undulation_m。
 */

#include "gnss_struct/range_obs.h"
#include "gnss_struct/sat_vis.h"
#include "gnss_struct/best_pos.h"

#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <cstddef>

namespace gnss {
namespace export_csv {

/**
 * @brief CSV导出器（线程安全）
 * @details 负责按需创建CSV文件、写入表头和数据行。
 *          每一类数据一个文件，文件名规则：
 *          {输出目录}/{前缀}_range.csv / _satvis.csv / _satvis2.csv / _bestpos.csv
 *
 *          文件惰性创建；写入失败会累加错误计数，可通过 had_write_error() /
 *          write_error_count() 查询。
 */
class CsvExporter {
public:
    /**
     * @brief 构造函数
     * @param output_dir 输出目录路径（不存在则自动创建）
     * @param file_prefix 文件名前缀，默认"gnss"
     */
    explicit CsvExporter(const std::string& output_dir,
                         const std::string& file_prefix = "gnss");

    ~CsvExporter();

    // 禁止拷贝，允许移动
    CsvExporter(const CsvExporter&) = delete;
    CsvExporter& operator=(const CsvExporter&) = delete;
    CsvExporter(CsvExporter&&) = delete;
    CsvExporter& operator=(CsvExporter&&) = delete;

    // ============================================================
    // 数据写入接口
    // ============================================================

    /**
     * @brief 写入一条RANGE观测帧到CSV
     * @param frame RANGE帧（含多颗卫星观测）
     * @note  每条观测展开为一行。
     *        列：GPS_Week, TOW_ms, Sat_System, Sat_PRN, GloFreq,
     *             Pseudorange_m, PsrStd_m, CarrierPhase_cycle, AdrStd_cycle,
     *             Doppler_Hz, CN0_dBHz, LockTime_s, ChTrStatus, Sat_System_Name
     *        ChTrStatus 为十六进制（如 0x08109C04）
     */
    void write_range_frame(const RangeFrame& frame);

    /**
     * @brief 写入一条SATVIS卫星可见性帧到CSV
     * @note  列：GPS_Week, TOW_ms, Sat_System, Sat_PRN, Elevation_deg,
     *             Azimuth_deg, Sat_System_Name
     *        （SATVIS(48) 为OEM6旧日志，解析器不再产生该帧）
     */
    void write_satvis_frame(const SatVisFrame& frame);

    /**
     * @brief 写入一条SATVIS2卫星可见性扩展帧到CSV
     * @note  列：GPS_Week, TOW_ms, Sat_System, Sat_PRN, Elevation_deg,
     *             Azimuth_deg, Health, GloFreq, TrueDoppler_Hz,
     *             ApparentDoppler_Hz, Sat_System_Name
     */
    void write_satvis2_frame(const SatVis2Frame& frame);

    /**
     * @brief 写入一条BESTPOS定位结果帧到CSV
     * @param frame BESTPOS帧
     * @note  每帧一行。
     *        列：GPS_Week, TOW_ms, Solution_Status, Position_Type, Latitude_deg,
     *             Longitude_deg, Height_m(MSL), Undulation_m, LatStd_m, LonStd_m,
     *             HgtStd_m, SVs_Tracked, SVs_Used
     */
    void write_bestpos_frame(const BestPosFrame& frame);

    /**
     * @brief 刷新所有已打开文件的缓冲区并关闭
     */
    void flush_and_close();

    /**
     * @brief 获取已写入的各类型行数统计
     */
    struct ExportStats {
        size_t range_rows = 0;
        size_t satvis_rows = 0;
        size_t satvis2_rows = 0;
        size_t bestpos_rows = 0;
    };

    [[nodiscard]] ExportStats stats() const noexcept { return export_stats_; }

    // ============================================================
    // 写入错误查询
    // ============================================================

    /**
     * @brief 是否发生过CSV写入（含打开/刷新/关闭）失败
     */
    [[nodiscard]] bool had_write_error() const noexcept;

    /**
     * @brief 累计的CSV写入失败次数
     */
    [[nodiscard]] size_t write_error_count() const noexcept;

private:
    /**
     * @brief 确保目录存在
     */
    static void ensure_directory(const std::string& dir);

    /**
     * @brief 惰性打开CSV文件（首次调用时创建文件并写入UTF-8 BOM + 表头）
     * @param file_stream 文件流引用
     * @param filepath 文件路径
     * @param header_line CSV表头行（不含换行符）
     * @return true=文件已就绪可写；false=打开失败（已累加错误计数）
     */
    bool ensure_csv_open(std::ofstream& file_stream,
                         const std::string& filepath,
                         const char* header_line);

    /**
     * @brief 检查流状态，失败则累加写入错误计数
     */
    void note_stream_state(const std::ofstream& file_stream) noexcept;

    /**
     * @brief 关闭单个流（刷盘 + 状态检查）
     */
    void close_stream(std::ofstream& file_stream) noexcept;

    /**
     * @brief 格式化GPS时间为文件名友好的字符串
     * @return "W2215_T345600000" 格式
     */
    static std::string format_time_stamp(const GpsTime& gt);

    std::string output_dir_;   ///< 输出目录
    std::string file_prefix_;  ///< 文件名前缀

    // 四个输出文件流（按类型分离，惰性创建）
    std::ofstream range_file_;
    std::ofstream satvis_file_;
    std::ofstream satvis2_file_;
    std::ofstream bestpos_file_;

    std::string range_filepath_;
    std::string satvis_filepath_;
    std::string satvis2_filepath_;
    std::string bestpos_filepath_;

    ExportStats export_stats_;     ///< 行数统计

    size_t write_error_count_ = 0; ///< 写入/打开/刷新失败累计次数
    bool   open_error_reported_ = false; ///< 是否已向stderr提示过打开失败
    bool   closed_ = false;        ///< flush_and_close 之后不再重新打开文件

    mutable std::mutex write_mutex_; ///< 写保护互斥锁（线程安全）
};

} // namespace export_csv
} // namespace gnss
