#pragma once
/**
 * @file    best_pos.h
 * @brief   BESTPOS最佳定位结果帧数据结构
 * @details 定义接收机PVT（位置/速度/时间）解算的最佳结果。
 *          BESTPOS 二进制日志的 Message ID = 42（OEM7手册 3.21 节 "BESTPOS"）。
 *          坐标系统默认为WGS-84大地坐标系。
 *
 *          @warning 高度字段 hgt 是 **Height above mean sea level（平均海平面
 *                   高度，MSL）**，不是椭球高。若要得到椭球高（HAE），需要
 *                   加上同帧的 undulation：hae = hgt + undulation。
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
 * @brief BESTPOSA定位结果帧（OEM7扩展格式，body固定72字节）
 * @details 包含接收机在一个历元的最佳位置解算结果。
 *          坐标系统为所选基准（datum_id，61=WGS84 / 63=USER）。
 *
 *          二进制帧格式（body部分，72字节；偏移相对body起始，H=帧头起始）：
 *          +0  [sol_stat: 4B Enum]        Solution status（Table 86）
 *          +4  [pos_type: 4B Enum]        Position type（Table 87）
 *          +8  [lat: 8B double]           Latitude (deg)
 *          +16 [lon: 8B double]           Longitude (deg)
 *          +24 [hgt: 8B double]           Height above mean sea level (m)
 *          +32 [undulation: 4B float]     大地水准面与椭球面之差 (m)
 *          +36 [datum_id: 4B Enum]        61=WGS84 / 63=USER
 *          +40 [lat_std: 4B float]        Latitude standard deviation (m)
 *          +44 [lon_std: 4B float]        Longitude standard deviation (m)
 *          +48 [hgt_std: 4B float]        Height standard deviation (m)
 *          +52 [stn_id: 4B char]          Base station ID
 *          +56 [diff_age: 4B float]       Differential age (s)
 *          +60 [sol_age: 4B float]        Solution age (s)
 *          +64 [num_svs: 1B]              Number of satellites tracked
 *          +65 [num_soln_svs: 1B]         Number of satellites used in solution
 *          +66 [num_soln_l1_svs: 1B]      # of satellites with L1/E1/B1 signals used
 *          +67 [num_soln_multi_svs: 1B]   # of satellites with multi-frequency signals used
 *          +68 [reserved1: 1B]            Reserved
 *          +69 [ext_sol_stat: 1B]         Extended solution status（Table 90）
 *          +70 [gal_bds_sig_mask: 1B]     Galileo and BeiDou signal-used mask（Table 89）
 *          +71 [gps_glo_sig_mask: 1B]     GPS and GLONASS signal-used mask（Table 88）
 *          +72 [crc: 4B]                  32-bit CRC（不属于body）
 *
 *          注：OEM6 时代的尾部字段清单（num_gg_l1 / num_gg_l1_l2 / num_gg_l1_l5 /
 *              reserved1 / num_glo_l1 / num_glo_l2 + 5B reserved，合计77字节）
 *              已被 OEM7 重新定义；OEM7 尾部只有上述 +64..+71 共 8 字节，body 总长 72。
 */
struct BestPosFrame {
    GpsTime gps_time;
    SolutionStatus solution_status = SolutionStatus::UNKNOWN;
    uint32_t position_type = 0;      ///< Position type（OEM7 Table 87）
    double latitude = 0.0;           ///< 纬度（度）
    double longitude = 0.0;          ///< 经度（度）
    double height = 0.0;             ///< 平均海平面(MSL)高度（米）；椭球高 = height + undulation
    float  undulation = 0.0f;        ///< 大地水准面起伏（米）
    uint32_t datum_id = 0;           ///< 基准编号（61=WGS84 / 63=USER）
    float  lat_std_dev = 0.0f;       ///< 纬度标准差（米）
    float  lon_std_dev = 0.0f;       ///< 经度标准差（米）
    float  hgt_std_dev = 0.0f;       ///< 高度标准差（米）
    uint8_t num_svs = 0;             ///< Number of satellites tracked
    uint8_t num_soln_svs = 0;        ///< Number of satellites used in solution

    BestPosFrame() = default;

    [[nodiscard]] bool is_valid() const noexcept {
        return solution_status != SolutionStatus::UNKNOWN;
    }

    /** @brief 是否为窄巷固定解（OEM7 Table 87: 50 = NARROW_INT） */
    [[nodiscard]] bool is_rtk_fixed() const noexcept {
        return position_type == 50;  // NARROW_INT
    }

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] float horizontal_precision() const noexcept;
    [[nodiscard]] std::string summary() const;
};

} // namespace gnss
