/**
 * @file    gnss_bands.cpp
 * @brief   GNSS频点枚举实现
 */

#include "gnss_struct/gnss_bands.h"

namespace gnss {

const char* band_to_string(FrequencyBand band) noexcept {
    switch (band) {
        // GPS 频点
        case FrequencyBand::L1CA:    return "L1CA";
        case FrequencyBand::L2C:     return "L2C";
        case FrequencyBand::L2P:     return "L2P";
        case FrequencyBand::L5Q:     return "L5Q";

        // 北斗 频点
        case FrequencyBand::B1D1:    return "B1D1";
        case FrequencyBand::B2D1:    return "B2D1";
        case FrequencyBand::B3D1:    return "B3D1";
        case FrequencyBand::B1C:     return "B1C";
        case FrequencyBand::B2a:     return "B2a";

        // GLONASS 频点
        case FrequencyBand::G1CA:    return "G1CA";
        case FrequencyBand::G2CA:    return "G2CA";

        // Galileo 频点
        case FrequencyBand::E1BC:    return "E1BC";
        case FrequencyBand::E5a:     return "E5a";
        case FrequencyBand::E5b:     return "E5b";

        default:                     return "UNKNOWN";
    }
}

} // namespace gnss
