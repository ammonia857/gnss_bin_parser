#pragma once
/**
 * @file    gnss_bands.h
 * @brief   GNSS信号频点定义
 * @details 定义各卫星系统的信号频点枚举，涵盖GPS L1/L2/L5，
 *          北斗B1I/B2I/B3I/B1C/B2a等常用频点。
 */

#include <cstdint>

namespace gnss {

/**
 * @brief GNSS信号频点枚举
 * @note  频点标识用于区分同一颗卫星在不同频率上的观测值。
 *         每条RANGE观测记录包含一个频点标识。
 *
 *         GPS频点：
 *         - L1CA  (1575.42MHz, C/A码)
 *         - L2C   (1227.60MHz, 民用L2)
 *         - L2P   (1227.60MHz, P码)
 *         - L5Q   (1176.45MHz, L5)
 *
 *         北斗频点：
 *         - B1D1  (1561.098MHz, B1I信号)
 *         - B2D1  (1207.14MHz, B2I/B2b信号)
 *         - B1C   (1575.42MHz, B1C信号)
 *         - B2a   (1176.45MHz, B2a信号)
 */
enum class FrequencyBand : uint8_t {
    // GPS 频点
    L1CA    = 0,   ///< GPS L1 C/A 1575.42MHz
    L2C     = 1,   ///< GPS L2C    1227.60MHz
    L2P     = 2,   ///< GPS L2P(Y) 1227.60MHz
    L5Q     = 3,   ///< GPS L5     1176.45MHz

    // 北斗(BDS)频点
    B1D1    = 10,  ///< BDS B1I    1561.098MHz
    B2D1    = 11,  ///< BDS B2I/B2b 1207.14MHz
    B3D1    = 12,  ///< BDS B3I    1268.52MHz
    B1C     = 13,  ///< BDS B1C    1575.42MHz
    B2a     = 14,  ///< BDS B2a    1176.45MHz

    // GLONASS 频点
    G1CA    = 20,  ///< GLO L1 C/A ~1602MHz + k*0.5625MHz
    G2CA    = 21,  ///< GLO L2 C/A ~1246MHz + k*0.4375MHz

    // Galileo 频点
    E1BC    = 30,  ///< GAL E1     1575.42MHz
    E5a     = 31,  ///< GAL E5a    1176.45MHz
    E5b     = 32,  ///< GAL E5b    1207.14MHz

    UNKNOWN_BAND = 0xFF
};

/**
 * @brief 频点枚举转人类可读字符串
 * @param band 频点枚举值
 * @return "L1CA" / "L2C" / "B1D1" / "B2D1" 等字符串
 */
[[nodiscard]] const char* band_to_string(FrequencyBand band) noexcept;

} // namespace gnss
