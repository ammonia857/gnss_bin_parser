#pragma once
/**
 * @file    best_pos.h
 * @brief   BESTPOS最佳定位结果帧数据结构
 * @details 定义接收机PVT（位置/速度/时间）解算的最佳结果。
 *          BESTPOS帧对应BIN中Message ID=300的消息。
 *          包含大地坐标（纬度/经度/椭球高）及精度指标。
 */

#include "gnss_time.h"
#include <string>

namespace gnss {

/**
 * @brief 定位解状态枚举（OEM7 Solution Status）
 * @note  表示接收机解算质量，与位置类型(position_type)不同。
 *        参考: OEM7手册 Table 86: Solution Status
 */
enum class SolutionStatus : uint32_t {
    SOL_COMPUTED      = 0,   ///< 解已算出
    INSUFFICIENT_OBS  = 1,   ///< 观测值不足
    NO_CONVERGENCE    = 2,   ///< 未收敛
    SINGULARITY       = 3,   ///< 参数矩阵奇异
    COV_TRACE         = 4,   ///< 协方差迹超限
    TEST_DIST         = 5,   ///< 检验距离超限
    COLD_START        = 6,   ///< 冷启动未收敛
    V_H_LIMIT         = 7,   ///< 高程/速度超限
    VARIANCE          = 8,   ///< 方差超限
    RESIDUALS         = 9,   ///< 残差过大
    INTEGRITY_WARNING = 13,  ///< 完整性警告
    PENDING           = 18,  ///< 等待中
    INVALID_FIX       = 19,  ///< 固定位置无效
    UNAUTHORIZED      = 20,  ///< 位置类型未授权
    UNKNOWN           = 0xFFFFFFFF
};

/**
 * @brief 定位解状态转字符串
 */
[[nodiscard]] const char* solution_status_to_string(SolutionStatus st) noexcept;

/**
 * @brief 定位类型转字符串
 */
[[nodiscard]] const char* position_type_to_string(uint32_t pos_type) noexcept;

/**
 * @brief BESTPOSA定位结果帧（OEM7扩展格式，72字节）
 * @details 包含接收机在一个历元的最佳位置解算结果。
 *          坐标系统为WGS-84大地坐标系。
 *
 *          二进制帧格式（body部分，72字节）：
 *          [sol_stat: 4B uint32] [pos_type: 4B uint32]
 *          [lat: 8B double] [lon: 8B double] [hgt: 8B double]
 *          [undulation: 4B float] [datum_id: 4B uint32]
 *          [lat_std: 4B float] [lon_std: 4B float] [hgt_std: 4B float]
 *          [stn_id: 4B] [diff_age: 4B float] [sol_age: 4B float]
 *          [num_svs: 1B] [num_soln_svs: 1B] [num_gg_l1: 1B] [num_gg_l1_l2: 1B]
 *          [num_gg_l1_l5: 1B] [reserved1: 1B] [num_glo_l1: 1B] [num_glo_l2: 1B]
 *          [reserved2: 5B]
 */
struct BestPosFrame {
    GpsTime gps_time;
    SolutionStatus solution_status = SolutionStatus::UNKNOWN;
    uint32_t position_type = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double height = 0.0;
    float  undulation = 0.0f;
    uint32_t datum_id = 0;
    float  lat_std_dev = 0.0f;
    float  lon_std_dev = 0.0f;
    float  hgt_std_dev = 0.0f;
    uint8_t num_svs = 0;
    uint8_t num_soln_svs = 0;

    BestPosFrame() = default;

    [[nodiscard]] bool is_valid() const noexcept {
        return solution_status != SolutionStatus::UNKNOWN;
    }

    [[nodiscard]] bool is_rtk_fixed() const noexcept {
        return position_type == 50;  // NARROW_INT
    }

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] float horizontal_precision() const noexcept;
    [[nodiscard]] std::string summary() const;
};

} // namespace gnss
