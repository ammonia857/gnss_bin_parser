#pragma once
/**
 * @file    bin_parser.h
 * @brief   BIN帧解析器
 * @details 负责从原始字节流中识别帧边界、解析帧头、提取消息体，
 *          并根据消息ID将消息体解析为对应的GNSS结构体。
 *          支持三种帧类型：RANGE(43)、SATVIS2(1043)、BESTPOS(42)。
 *          注：SATVIS(48) 为 OEM6 旧日志，OEM7 已由 SATVIS2 取代，本解析器不再解析，
 *              遇到时计入 ParseStats::unsupported_frames 并提示一次。
 */

#include "frame_header.h"
#include "gnss_struct/range_obs.h"
#include "gnss_struct/sat_vis.h"
#include "gnss_struct/best_pos.h"
#include "bin_io/bin_reader.h"

#include <vector>
#include <functional>
#include <cstdint>

namespace gnss {
namespace parser {

/**
 * @brief BIN帧解析器
 * @details 管理整个BIN文件的逐帧解析流程。
 *          使用回调函数模式——每解析出一帧立即回调通知，避免大量数据在内存堆积。
 *
 *          典型用法：
 *          @code
 *          MmapFile file("data.bin");
 *          BinParser parser;
 *          parser.set_on_range([](const RangeFrame& f) { ... });
 *          parser.set_on_satvis([](const SatVisFrame& f) { ... });
 *          parser.set_on_bestpos([](const BestPosFrame& f) { ... });
 *          auto stats = parser.parse(file);
 *          @endcode
 */
class BinParser {
public:
    /// 解析统计信息
    struct ParseStats {
        size_t total_frames = 0;
        size_t range_frames = 0;
        size_t satvis_frames = 0;
        size_t satvis2_frames = 0;
        size_t bestpos_frames = 0;
        size_t sync_lost_count = 0;
        size_t crc_error_count = 0;
        size_t bytes_processed = 0;
        uint64_t malformed_frames = 0;   ///< 帧头/消息体长度自洽性校验失败等结构性错误帧数
        uint64_t unsupported_frames = 0; ///< 结构合法但本工具不解析的日志帧数（含 SATVIS 旧日志）

        void reset() noexcept {
            total_frames = 0;
            range_frames = 0;
            satvis_frames = 0;
            satvis2_frames = 0;
            bestpos_frames = 0;
            sync_lost_count = 0;
            crc_error_count = 0;
            bytes_processed = 0;
            malformed_frames = 0;
            unsupported_frames = 0;
        }
    };

    // ============================================================
    // 回调函数类型定义
    // ============================================================

    /// RANGE观测帧回调：每解析出一个完整RANGE帧即调用
    using RangeCallback = std::function<void(const RangeFrame&)>;

    /// SATVIS卫星可见性帧回调
    using SatVisCallback = std::function<void(const SatVisFrame&)>;

    /// SATVIS2卫星可见性扩展帧回调
    using SatVis2Callback = std::function<void(const SatVis2Frame&)>;

    /// BESTPOS定位结果帧回调
    using BestPosCallback = std::function<void(const BestPosFrame&)>;

    /// 解析进度回调：(已处理字节数, 文件总大小)
    using ProgressCallback = std::function<void(size_t, size_t)>;

    /// 错误/警告回调：(级别, 消息)
    using LogCallback = std::function<void(const std::string&)>;

    BinParser() = default;

    // ============================================================
    // 回调注册
    // ============================================================

    void set_on_range(RangeCallback cb)   { on_range_   = std::move(cb); }
    void set_on_satvis(SatVisCallback cb) { on_satvis_  = std::move(cb); }
    void set_on_satvis2(SatVis2Callback cb) { on_satvis2_ = std::move(cb); }
    void set_on_bestpos(BestPosCallback cb) { on_bestpos_ = std::move(cb); }
    void set_on_progress(ProgressCallback cb) { on_progress_ = std::move(cb); }
    void set_on_log(LogCallback cb) { on_log_ = std::move(cb); }

    // ============================================================
    // 核心解析接口
    // ============================================================

    /**
     * @brief 解析整个BIN文件
     * @param mmap_file 已打开的内存映射文件
     * @return 解析统计信息
     * @throws std::runtime_error 文件读取失败时抛出
     *
     * @note  内部采用分块扫描策略：
     *        1. 逐chunk读取，在chunk边界附近保留重叠区避免漏帧
     *        2. 搜索同步头0xAA 0x44 0x12定位帧边界
     *        3. 解析帧头→根据MsgID分发到对应解析函数
     *        4. 通过回调将解析结果传递给上层
     */
    ParseStats parse(bin_io::MmapFile& mmap_file);

    /**
     * @brief 获取最近一次解析的统计信息
     */
    const ParseStats& stats() const noexcept { return stats_; }

private:
    /**
     * @brief 验证同步字节序列
     * @param data 指向候选同步位置的数据
     * @return true=同步头有效
     */
    static bool verify_sync(const uint8_t* data) noexcept;

    /**
     * @brief 计算CRC32校验值（与帧尾CRC对比）
     * @param data 数据起始（从sync开始）
     * @param length 数据长度（含帧头+body，不含CRC本身）
     * @return CRC32值
     */
    static uint32_t calculate_crc32(const uint8_t* data, size_t length) noexcept;

    /**
     * @brief 写日志
     */
    void log(const std::string& msg);

    // 回调函数对象
    RangeCallback    on_range_;
    SatVisCallback   on_satvis_;
    SatVis2Callback  on_satvis2_;
    BestPosCallback  on_bestpos_;
    ProgressCallback on_progress_;
    LogCallback      on_log_;
    ParseStats       stats_;
};

} // namespace parser
} // namespace gnss
