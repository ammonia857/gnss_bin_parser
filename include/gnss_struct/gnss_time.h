#pragma once
/**
 * @file    gnss_time.h
 * @brief   GNSS时间基类定义
 * @details 定义GPS时间结构体——GPS Week + 周内毫秒(TOW)。
 *          GPS Week 从1980年1月6日起算，每1024周翻转一次。
 *          示例：GPS Week 2215 + 毫秒 代表某一具体观测时刻。
 */

#include <cstdint>
#include <string>

namespace gnss {

/**
 * @brief GPS时间结构体
 * @note  GPS周(week)：从1980年1月6日（GPS纪元）起算的整周数。
 *        周内毫秒(tow_ms)：本周内的毫秒计数，范围 [0, 604799999]。
 *        组合可精确到毫秒级定位。
 *
 *        示例：Week=2215, tow_ms=345600000 表示第2215周星期一00:00:00.000
 */
struct GpsTime {
    uint16_t week = 0;    ///< GPS周编号（0~65535，满1024自动翻转）
    uint32_t tow_ms = 0;  ///< GPS周内毫秒（0~604799999，即0ms~6天23小时59分59秒999毫秒）

    GpsTime() = default;
    GpsTime(uint16_t w, uint32_t t) : week(w), tow_ms(t) {}

    /**
     * @brief 格式化为可读字符串
     * @return "Week=2215, TOW=345600000ms" 格式
     */
    [[nodiscard]] std::string to_string() const;

    /**
     * @brief 将周内毫秒转换为日时分秒
     * @return 结构体包含 day_of_week, hour, minute, second, millisecond
     */
    struct TimeBreakdown {
        int day_of_week;  ///< 周日=0, 周一=1, ..., 周六=6
        int hour;
        int minute;
        int second;
        int millisecond;
    };

    [[nodiscard]] TimeBreakdown breakdown() const noexcept;
};

/**
 * @brief 卫星星座系统枚举
 */
enum class SatelliteSystem : uint8_t {
    GPS = 0,      ///< 美国GPS系统
    BDS = 1,      ///< 中国北斗系统（BDS）
    GLONASS = 2,  ///< 俄罗斯GLONASS
    GALILEO = 3,  ///< 欧盟Galileo
    SBAS = 4,     ///< 星基增强系统
    QZSS = 5,     ///< 日本准天顶
    UNKNOWN = 0xFF
};

/**
 * @brief 卫星系统转中文名称
 * @param sys 卫星系统枚举
 * @return "GPS" / "北斗" / "GLONASS" / "Galileo" / "SBAS" / "QZSS" / "未知"
 */
[[nodiscard]] const char* system_to_name(SatelliteSystem sys) noexcept;

/**
 * @brief 卫星系统转单字符标识
 * @param sys 卫星系统枚举
 * @return 'G'/'C'/'R'/'E'/'S'/'J'/'?'
 */
[[nodiscard]] char system_to_char(SatelliteSystem sys) noexcept;

} // namespace gnss
