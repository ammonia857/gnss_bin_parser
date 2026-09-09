/**
 * @file    sat_vis.cpp
 * @brief   SATVIS卫星可见性帧结构体实现（OEM7）
 */

#include "gnss_struct/sat_vis.h"
#include <sstream>
#include <iomanip>

namespace gnss {

std::string SatVisibility::to_string() const {
    std::ostringstream oss;
    oss << "PRN=" << std::setw(2) << std::setfill('0') << static_cast<int>(sat_prn)
        << " EL=" << std::fixed << std::setprecision(1) << elevation << "°"
        << " AZ=" << std::fixed << std::setprecision(1) << azimuth << "°";
    return oss.str();
}

int SatVisFrame::count_above_elevation(float min_elev) const {
    int count = 0;
    for (const auto& sat : sats) {
        if (sat.elevation >= min_elev) {
            ++count;
        }
    }
    return count;
}

std::string SatVisFrame::summary() const {
    if (!is_valid()) return "SATVIS帧无效";

    std::ostringstream oss;
    oss << "[SATVIS] " << gps_time.to_string()
        << " 系统=" << system_to_name(sat_system)
        << " 总卫星=" << static_cast<int>(total_sats);

    // 统计仰角分布
    int above_15 = count_above_elevation(15.0f);
    int above_5  = count_above_elevation(5.0f);
    oss << " (EL>15°=" << above_15 << ", EL>5°=" << above_5 << ")";
    return oss.str();
}

// ============================================================
// SATVIS2 实现
// ============================================================

std::string SatVis2Entry::to_string() const {
    std::ostringstream oss;
    oss << "PRN=" << std::setw(2) << std::setfill('0') << sat_prn
        << " EL=" << std::fixed << std::setprecision(1) << elevation << "°"
        << " AZ=" << std::fixed << std::setprecision(1) << azimuth << "°";
    return oss.str();
}

int SatVis2Frame::count_above_elevation(double min_elev) const {
    int count = 0;
    for (const auto& sat : sats) {
        if (sat.elevation >= min_elev) {
            ++count;
        }
    }
    return count;
}

std::string SatVis2Frame::summary() const {
    if (!is_valid()) return "SATVIS2帧无效";

    std::ostringstream oss;
    oss << "[SATVIS2] " << gps_time.to_string()
        << " 系统=" << system_to_name(sat_system)
        << " 总卫星=" << num_sats;

    int above_15 = count_above_elevation(15.0);
    int above_5  = count_above_elevation(5.0);
    oss << " (EL>15°=" << above_15 << ", EL>5°=" << above_5 << ")";
    return oss.str();
}

} // namespace gnss
