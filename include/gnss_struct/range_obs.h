#pragma once
/**
 * @file    range_obs.h
 * @brief   RANGE观测日志帧数据结构
 * @details 定义单条卫星伪距/载波相位观测值结构和完整的RANGE帧。
 *          RANGE帧对应BIN中Message ID=43的消息。
 */

#include "gnss_time.h"
#include "gnss_bands.h"
#include <vector>
#include <string>

namespace gnss {

/**
 * @brief 单条卫星观测值（NovAtel OEM7 RANGEA格式，44字节）
 * @note  包含一颗卫星在一个频点上的完整观测数据。
 *        伪距单位为米，载波相位单位为周(cycle)。
 *        卫星编号从1开始，GPS 1~32，北斗 1~63。
 */
struct RangeObservation {
    SatelliteSystem sat_system;   ///< 卫星星座（由 ch_tr_status 的 bit16-18 映射得到）
    uint16_t        sat_prn;      ///< 卫星PRN编号（GPS 1~32，北斗 1~63）
    uint16_t        glofreq;      ///< GLONASS频率偏置；官方语义：值为 (GLONASS Frequency + 7)，非GLONASS为0
    double          pseudorange;  ///< 伪距观测值（米）
    float           psr_std;      ///< 伪距标准差（米）
    double          carrier_phase;///< 载波相位观测值（周，cycle）
    float           adr_std;      ///< 载波相位标准差（周）
    float           doppler;      ///< 多普勒频移（Hz）
    float           cn0;          ///< 载噪比（dB-Hz）
    float           locktime;     ///< 锁定时间（秒）
    uint32_t        ch_tr_status; ///< 信道跟踪状态

    [[nodiscard]] std::string sat_label() const;
    [[nodiscard]] std::string to_string() const;
};

/**
 * @brief RANGE观测日志帧（OEM7 RANGEA格式）
 * @details BIN中每条RANGE帧包含一个历元的多颗卫星观测数据。
 *
 *          二进制帧格式（body部分）：
 *          [观测数: uint32(4B)] [观测1: 44B] [观测2: 44B] ...
 *          每条观测(44B): prn(2B)+glofreq(2B)+psr(8B double)+psr_std(4B float)
 *                         +adr(8B double)+adr_std(4B float)+dopp(4B float)
 *                         +cn0(4B float)+locktime(4B float)+ch_tr_status(4B uint32)
 */
struct RangeFrame {
    GpsTime gps_time;                    ///< GPS观测时刻
    uint32_t num_observations = 0;       ///< 本帧观测值数量
    std::vector<RangeObservation> observations;

    RangeFrame() = default;

    [[nodiscard]] bool is_valid() const noexcept {
        return num_observations > 0 && !observations.empty();
    }

    [[nodiscard]] std::string summary() const;
};

} // namespace gnss
