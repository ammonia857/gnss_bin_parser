#include "gnss_struct/best_pos.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace gnss {

const char* solution_status_to_string(SolutionStatus st) noexcept {
    switch (st) {
        case SolutionStatus::SOL_COMPUTED:       return "SOL_COMPUTED";
        case SolutionStatus::INSUFFICIENT_OBS:   return "INSUFFICIENT_OBS";
        case SolutionStatus::NO_CONVERGENCE:     return "NO_CONVERGENCE";
        case SolutionStatus::SINGULARITY:        return "SINGULARITY";
        case SolutionStatus::COV_TRACE:          return "COV_TRACE";
        case SolutionStatus::TEST_DIST:          return "TEST_DIST";
        case SolutionStatus::COLD_START:         return "COLD_START";
        case SolutionStatus::V_H_LIMIT:          return "V_H_LIMIT";
        case SolutionStatus::VARIANCE:           return "VARIANCE";
        case SolutionStatus::RESIDUALS:          return "RESIDUALS";
        case SolutionStatus::INTEGRITY_WARNING:  return "INTEGRITY_WARNING";
        case SolutionStatus::PENDING:            return "PENDING";
        case SolutionStatus::INVALID_FIX:        return "INVALID_FIX";
        case SolutionStatus::UNAUTHORIZED:       return "UNAUTHORIZED";
        default:                                  return "UNKNOWN";
    }
}

const char* position_type_to_string(uint32_t pos_type) noexcept {
    // 完整映射表：OEM7 Commands and Logs Reference Manual v22,
    // Chapter 3 Logs, "Table 87: Position or Velocity Type"（BESTPOS 的 pos type 字段）。
    // 手册中明确列出二进制的取值；标注 Reserved 的区间返回 UNKNOWN。
    switch (pos_type) {
        case 0:  return "NONE";
        case 1:  return "FIXEDPOS";
        case 2:  return "FIXEDHEIGHT";
        // 3-7 Reserved
        case 8:  return "DOPPLER_VELOCITY";
        // 9-15 Reserved
        case 16: return "SINGLE";
        case 17: return "PSRDIFF";
        case 18: return "WAAS";
        case 19: return "PROPAGATED";
        // 20-31 Reserved
        case 32: return "L1_FLOAT";
        // 33 Reserved
        case 34: return "NARROW_FLOAT";
        // 35-47 Reserved
        case 48: return "L1_INT";
        case 49: return "WIDE_INT";
        case 50: return "NARROW_INT";
        case 51: return "RTK_DIRECT_INS";
        case 52: return "INS_SBAS";
        case 53: return "INS_PSRSP";
        case 54: return "INS_PSRDIFF";
        case 55: return "INS_RTKFLOAT";
        case 56: return "INS_RTKFIXED";
        // 57-67 Reserved
        case 68: return "PPP_CONVERGING";
        case 69: return "PPP";
        case 70: return "OPERATIONAL";
        case 71: return "WARNING";
        case 72: return "OUT_OF_BOUNDS";
        case 73: return "INS_PPP_CONVERGING";
        case 74: return "INS_PPP";
        // 75-76 手册 Table 87 未列出（分页处空缺），按未定义处理
        case 77: return "PPP_BASIC_CONVERGING";
        case 78: return "PPP_BASIC";
        case 79: return "INS_PPP_BASIC_CONVERGING";
        case 80: return "INS_PPP_BASIC";
        default: return "UNKNOWN";
    }
}

std::string BestPosFrame::to_string() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(9)
        << "lat=" << latitude << "° "
        << "lon=" << longitude << "° "
        << "hgt=" << std::setprecision(4) << height << "m";
    return oss.str();
}

float BestPosFrame::horizontal_precision() const noexcept {
    return std::sqrt(lat_std_dev * lat_std_dev + lon_std_dev * lon_std_dev);
}

std::string BestPosFrame::summary() const {
    std::ostringstream oss;
    oss << "[BESTPOS] " << gps_time.to_string()
        << " 解类型=" << position_type_to_string(position_type)
        << " 解状态=" << solution_status_to_string(solution_status)
        << " " << to_string()
        << " HPrec=" << std::fixed << std::setprecision(3)
        << horizontal_precision() << "m"
        << " SVs=" << static_cast<int>(num_svs)
        << "/" << static_cast<int>(num_soln_svs);
    return oss.str();
}

} // namespace gnss
