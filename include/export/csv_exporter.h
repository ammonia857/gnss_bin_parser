#pragma once
/**
 * @file    csv_exporter.h
 * @brief   解析结果CSV导出器
 * @details 将BIN解析出的GNSS结构化数据按时间分段导出为CSV文本文件。
 *          支持三种输出：
 *          1. RANGE观测数据CSV：伪距/载波相位
 *          2. SATVIS卫星可见性CSV：仰角/方位角
 *          3. BESTPOS定位结果CSV：纬度/经度/大地高
 *
 *          按GPS周内时间戳排序输出，每个文件包含CSV表头行。
 *          CSV使用UTF-8编码，兼容Excel/WPS直接打开。
 */

#include "gnss_struct/range_obs.h"
#include "gnss_struct/sat_vis.h"
#include "gnss_struct/best_pos.h"

#include <fstream>
#include <string>
#include <vector>
#include <mutex>

namespace gnss {
namespace export_csv {

/**
 * @brief CSV导出器（线程安全）
 * @details 负责创建CSV文件、写入表头和数据行。
 *          按消息类型分别输出到不同文件，文件名包含类型和时间信息。
 *
 *          输出文件命名规则：
 *          {输出目录}/{前缀}_range_{起始时间}.csv
 *          {输出目录}/{前缀}_satvis_{起始时间}.csv
 *          {输出目录}/{前缀}_bestpos_{起始时间}.csv
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
     *        列：GPS_Week, TOW_ms, Sat_System, Sat_PRN, Freq_Band,
     *             Pseudorange_m, CarrierPhase_cycle
     */
    void write_range_frame(const RangeFrame& frame);

    /**
     * @brief 写入一条SATVIS卫星可见性帧到CSV
     */
    void write_satvis_frame(const SatVisFrame& frame);

    /**
     * @brief 写入一条SATVIS2卫星可见性扩展帧到CSV
     */
    void write_satvis2_frame(const SatVis2Frame& frame);

    /**
     * @brief 写入一条BESTPOS定位结果帧到CSV
     * @param frame BESTPOS帧
     * @note  每帧一行。
     *        列：GPS_Week, TOW_ms, Solution_Status, Latitude_deg,
     *             Longitude_deg, Height_m, LatStd_m, LonStd_m, HgtStd_m
     */
    void write_bestpos_frame(const BestPosFrame& frame);

    /**
     * @brief 刷新所有文件缓冲区并关闭
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

private:
    /**
     * @brief 确保目录存在
     */
    static void ensure_directory(const std::string& dir);

    /**
     * @brief 打开/确保CSV文件已打开并写入表头
     * @param file_stream 文件流引用
     * @param filepath 文件路径
     * @param header_line CSV表头行（不含换行符）
     */
    static void open_csv(std::ofstream& file_stream,
                         const std::string& filepath,
                         const std::string& header_line);

    /**
     * @brief 格式化GPS时间为文件名友好的字符串
     * @return "W2215_T345600000" 格式
     */
    static std::string format_time_stamp(const GpsTime& gt);

    std::string output_dir_;   ///< 输出目录
    std::string file_prefix_;  ///< 文件名前缀

    // 四个输出文件流（按类型分离）
    std::ofstream range_file_;
    std::ofstream satvis_file_;
    std::ofstream satvis2_file_;
    std::ofstream bestpos_file_;

    std::string range_filepath_;
    std::string satvis_filepath_;
    std::string satvis2_filepath_;
    std::string bestpos_filepath_;

    ExportStats export_stats_;     ///< 行数统计

    std::mutex write_mutex_;       ///< 写保护互斥锁（线程安全）
};

} // namespace export_csv
} // namespace gnss
