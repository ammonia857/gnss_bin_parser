#include "gnss_struct/range_obs.h"
#include <sstream>
#include <iomanip>

namespace gnss {

std::string RangeObservation::sat_label() const {
    char sys_char = '?';
    if (sat_prn <= 37 && glofreq == 0) sys_char = 'G';
    else if (sat_prn <= 63) sys_char = 'C';
    else if (glofreq > 0) sys_char = 'R';
    else sys_char = 'E';

    std::ostringstream oss;
    oss << sys_char << std::setw(2) << std::setfill('0') << sat_prn;
    return oss.str();
}

std::string RangeObservation::to_string() const {
    std::ostringstream oss;
    oss << sat_label() << " "
        << "PR=" << std::fixed << std::setprecision(3) << pseudorange << " "
        << "CP=" << std::fixed << std::setprecision(3) << carrier_phase << " "
        << "CN0=" << std::fixed << std::setprecision(1) << cn0;
    return oss.str();
}

std::string RangeFrame::summary() const {
    if (!is_valid()) return "RANGE帧无效";

    std::ostringstream oss;
    oss << "[RANGE] " << gps_time.to_string()
        << " 观测数=" << num_observations;
    return oss.str();
}

} // namespace gnss
