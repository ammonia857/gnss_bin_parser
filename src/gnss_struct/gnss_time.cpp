/**
 * @file    gnss_time.cpp
 * @brief   GNSS时间结构体和卫星系统枚举的实现
 */

#include "gnss_struct/gnss_time.h"
#include <sstream>
#include <iomanip>

namespace gnss {

std::string GpsTime::to_string() const {
    std::ostringstream oss;
    oss << "Week=" << week << ", TOW=" << tow_ms << "ms";
    return oss.str();
}

GpsTime::TimeBreakdown GpsTime::breakdown() const noexcept {
    TimeBreakdown tb{};

    // 周内毫秒 → 各时间分量
    const uint32_t ms_per_day    = 86400000;  // 24 * 60 * 60 * 1000
    const uint32_t ms_per_hour   = 3600000;   // 60 * 60 * 1000
    const uint32_t ms_per_minute = 60000;     // 60 * 1000
    const uint32_t ms_per_second = 1000;

    uint32_t remaining = tow_ms;

    tb.day_of_week = static_cast<int>(remaining / ms_per_day);
    remaining %= ms_per_day;

    tb.hour = static_cast<int>(remaining / ms_per_hour);
    remaining %= ms_per_hour;

    tb.minute = static_cast<int>(remaining / ms_per_minute);
    remaining %= ms_per_minute;

    tb.second = static_cast<int>(remaining / ms_per_second);
    tb.millisecond = static_cast<int>(remaining % ms_per_second);

    return tb;
}

const char* system_to_name(SatelliteSystem sys) noexcept {
    switch (sys) {
        case SatelliteSystem::GPS:      return "GPS";
        case SatelliteSystem::BDS:      return "北斗";
        case SatelliteSystem::GLONASS:  return "GLONASS";
        case SatelliteSystem::GALILEO:  return "Galileo";
        case SatelliteSystem::SBAS:     return "SBAS";
        case SatelliteSystem::QZSS:     return "QZSS";
        case SatelliteSystem::NAVIC:    return "NavIC";
        case SatelliteSystem::OTHER:    return "其它";
        case SatelliteSystem::UNKNOWN:  return "未知";
        default:                        return "未知";
    }
}

char system_to_char(SatelliteSystem sys) noexcept {
    switch (sys) {
        case SatelliteSystem::GPS:      return 'G';
        case SatelliteSystem::BDS:      return 'C';
        case SatelliteSystem::GLONASS:  return 'R';
        case SatelliteSystem::GALILEO:  return 'E';
        case SatelliteSystem::SBAS:     return 'S';
        case SatelliteSystem::QZSS:     return 'J';
        case SatelliteSystem::NAVIC:    return 'I';
        case SatelliteSystem::OTHER:    return '?';
        case SatelliteSystem::UNKNOWN:  return '?';
        default:                        return '?';
    }
}

SatelliteSystem system_from_ch_tr_status(uint32_t ch_tr_status) noexcept {
    // ch-tr-status 的 bit16-18 为星座（0x00070000），bit21-25 为信号类型
    switch ((ch_tr_status >> 16) & 0x07U) {
        case 0:  return SatelliteSystem::GPS;
        case 1:  return SatelliteSystem::GLONASS;
        case 2:  return SatelliteSystem::SBAS;
        case 3:  return SatelliteSystem::GALILEO;
        case 4:  return SatelliteSystem::BDS;
        case 5:  return SatelliteSystem::QZSS;
        case 6:  return SatelliteSystem::NAVIC;
        case 7:  return SatelliteSystem::OTHER;
        default: return SatelliteSystem::UNKNOWN;
    }
}

SatelliteSystem system_from_log_enum(uint32_t value) noexcept {
    // 日志字段 "Satellite System"（OEM7 Table 124）：4/3/8 为空缺值 → UNKNOWN
    switch (value) {
        case 0:  return SatelliteSystem::GPS;
        case 1:  return SatelliteSystem::GLONASS;
        case 2:  return SatelliteSystem::SBAS;
        case 5:  return SatelliteSystem::GALILEO;
        case 6:  return SatelliteSystem::BDS;
        case 7:  return SatelliteSystem::QZSS;
        case 9:  return SatelliteSystem::NAVIC;
        default: return SatelliteSystem::UNKNOWN;
    }
}

} // namespace gnss
