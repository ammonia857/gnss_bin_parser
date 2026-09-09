#pragma once
/**
 * @file    sat_vis.h
 * @brief   SATVIS卫星可见性帧数据结构（NovAtel OEM7）
 * @details 定义单颗卫星的天线方位角和仰角信息，以及完整的SATVIS帧。
 *          SATVIS帧对应OEM7 BIN中Message ID=48的消息。
 */

#include "gnss_time.h"
#include <vector>
#include <string>

namespace gnss {

/**
 * @brief 单颗卫星的可见性信息
 * @note  仰角(elevation)和方位角(azimuth)均以度(°)为单位。
 *        仰角范围 0°~90°（地平线~天顶）。
 *        方位角范围 0°~360°（北→东→南→西→北）。
 *
 *        截断高度角：通常在5°~15°以上卫星才有效参与解算。
 */
struct SatVisibility {
    uint8_t sat_prn;      ///< 卫星PRN编号
    float   elevation;    ///< 仰角（度），0°=地平线，90°=天顶
    float   azimuth;      ///< 方位角（度），0°=北，90°=东，180°=南，270°=西

    /**
     * @brief 检查卫星仰角是否高于截断高度（默认5°）
     * @param min_elev 最低有效仰角（度）
     * @return true=高于截断角，可参与定位
     */
    [[nodiscard]] bool above_mask(float min_elev = 5.0f) const noexcept {
        return elevation >= min_elev;
    }

    /** @brief 格式化为字符串 "PRN=01 EL=45.2° AZ=120.8°" */
    [[nodiscard]] std::string to_string() const;
};

/**
 * @brief SATVIS卫星可见性帧（完整帧，OEM7 ID=48）
 * @details 每个历元包含一个卫星系统在某时刻的所有可见卫星信息。
 *          用于星历预测、选星策略和定位解算的卫星筛选。
 *
 *          二进制帧格式（body部分）：
 *          [卫星系统: 1B] [卫星总数: 1B] [保留: 2B]
 *          [卫星1: 10B] [卫星2: 10B] ...
 *          单颗卫星：sat_prn(1B)+reserved(1B)+elevation(4B float)+azimuth(4B float)
 */
struct SatVisFrame {
    GpsTime gps_time;                  ///< GPS观测时刻
    SatelliteSystem sat_system;         ///< 所属卫星星座
    uint8_t total_sats = 0;            ///< 本帧卫星总数
    std::vector<SatVisibility> sats;   ///< 卫星可见性列表

    SatVisFrame() = default;

    /** @brief 检查帧是否包含有效数据 */
    [[nodiscard]] bool is_valid() const noexcept {
        return total_sats > 0 && !sats.empty();
    }

    /**
     * @brief 统计仰角高于指定角度的卫星数
     * @param min_elev 最低仰角（度）
     * @return 高于min_elev的卫星数
     */
    [[nodiscard]] int count_above_elevation(float min_elev = 5.0f) const;

    /** @brief 生成帧摘要字符串 */
    [[nodiscard]] std::string summary() const;
};

/**
 * @brief SATVIS2单颗卫星可见性信息（OEM7扩展格式，40字节）
 */
struct SatVis2Entry {
    uint16_t sat_prn;         ///< 卫星PRN编号
    int16_t  glofreq;         ///< GLONASS频率通道（非GLONASS为0）
    uint32_t health;          ///< 卫星健康状态
    double   elevation;       ///< 仰角（度）
    double   azimuth;         ///< 方位角（度）
    double   true_doppler;    ///< 理论多普勒（Hz）
    double   apparent_doppler;///< 视多普勒（Hz，含钟漂修正）

    [[nodiscard]] bool above_mask(double min_elev = 5.0) const noexcept {
        return elevation >= min_elev;
    }

    [[nodiscard]] std::string to_string() const;
};

/**
 * @brief SATVIS2卫星可见性帧（OEM7 ID=1043）
 * @details 每个历元包含一个卫星系统的所有可见卫星信息。
 *
 *          二进制帧格式（body部分，16+N*40字节）：
 *          [system_id: 4B uint32] [sat_vis_valid: 4B] [almanac_flag: 4B]
 *          [num_sats: 4B uint32]
 *          [卫星1: 40B] [卫星2: 40B] ...
 *          单颗卫星(40B): sat_id(4B, 低16位PRN+高16位glofreq)+health(4B)
 *                         +elev(8B double)+az(8B double)
 *                         +true_doppler(8B double)+apparent_doppler(8B double)
 */
struct SatVis2Frame {
    GpsTime gps_time;
    SatelliteSystem sat_system = SatelliteSystem::UNKNOWN;
    bool    sat_vis_valid = false;
    bool    almanac_flag  = false;
    uint32_t num_sats = 0;
    std::vector<SatVis2Entry> sats;

    SatVis2Frame() = default;

    [[nodiscard]] bool is_valid() const noexcept {
        return num_sats > 0 && !sats.empty();
    }

    [[nodiscard]] int count_above_elevation(double min_elev = 5.0) const;
    [[nodiscard]] std::string summary() const;
};

} // namespace gnss
