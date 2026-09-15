#pragma once
/**
 * @file    endian_utils.h
 * @brief   GNSS BIN文件字节序转换工具
 * @details 提供大端/小端字节序互转。NovAtel OEM7 BIN 消息为小端存储
 *          （帧尾 CRC32 亦为小端），因此解析路径统一使用 little_to_host_* 与
 *          load_*_le 系列读取器；big_to_host_* 仅保留给其它大端数据源使用。
 *          所有函数均为inline，零开销抽象。
 */

#include <cstdint>
#include <cstring>

namespace gnss {
namespace bin_io {

/**
 * @brief 检测当前主机是否为小端字节序
 * @return true=小端, false=大端
 */
inline bool is_little_endian() noexcept {
    constexpr uint16_t test_val = 0x0001;
    return (*reinterpret_cast<const uint8_t*>(&test_val) == 0x01);
}

/**
 * @brief 翻转16位无符号整数的字节序
 * @param val 输入值
 * @return 字节序翻转后的值
 */
inline constexpr uint16_t swap_uint16(uint16_t val) noexcept {
    return static_cast<uint16_t>((val << 8) | (val >> 8));
}

/**
 * @brief 翻转32位无符号整数的字节序
 * @param val 输入值
 * @return 字节序翻转后的值
 */
inline constexpr uint32_t swap_uint32(uint32_t val) noexcept {
    return ((val << 24) & 0xFF000000U) |
           ((val << 8)  & 0x00FF0000U) |
           ((val >> 8)  & 0x0000FF00U) |
           ((val >> 24) & 0x000000FFU);
}

/**
 * @brief 翻转64位无符号整数的字节序
 * @param val 输入值
 * @return 字节序翻转后的值
 */
inline constexpr uint64_t swap_uint64(uint64_t val) noexcept {
    return ((val << 56) & 0xFF00000000000000ULL) |
           ((val << 40) & 0x00FF000000000000ULL) |
           ((val << 24) & 0x0000FF0000000000ULL) |
           ((val << 8)  & 0x000000FF00000000ULL) |
           ((val >> 8)  & 0x00000000FF000000ULL) |
           ((val >> 24) & 0x0000000000FF0000ULL) |
           ((val >> 40) & 0x000000000000FF00ULL) |
           ((val >> 56) & 0x00000000000000FFULL);
}

/**
 * @brief 翻转32位浮点数（IEEE 754）的字节序
 * @param val 输入浮点值
 * @return 字节序翻转后的浮点值
 */
inline float swap_float(float val) noexcept {
    uint32_t raw = 0;
    std::memcpy(&raw, &val, sizeof(raw));
    raw = swap_uint32(raw);
    float result = 0.0f;
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

/**
 * @brief 翻转64位双精度浮点数的字节序
 * @param val 输入双精度值
 * @return 字节序翻转后的双精度值
 */
inline double swap_double(double val) noexcept {
    uint64_t raw = 0;
    std::memcpy(&raw, &val, sizeof(raw));
    raw = swap_uint64(raw);
    double result = 0.0;
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

// ============================================================
// 大端→主机 转换函数（BIN文件为大端存储时使用）
// ============================================================

inline uint16_t big_to_host_u16(uint16_t big_val) noexcept {
    return is_little_endian() ? swap_uint16(big_val) : big_val;
}

inline uint32_t big_to_host_u32(uint32_t big_val) noexcept {
    return is_little_endian() ? swap_uint32(big_val) : big_val;
}

inline uint64_t big_to_host_u64(uint64_t big_val) noexcept {
    return is_little_endian() ? swap_uint64(big_val) : big_val;
}

inline float big_to_host_float(float big_val) noexcept {
    return is_little_endian() ? swap_float(big_val) : big_val;
}

inline double big_to_host_double(double big_val) noexcept {
    return is_little_endian() ? swap_double(big_val) : big_val;
}

// ============================================================
// 小端→主机 转换函数（OEM7 BIN文件实际为小端存储）
// ============================================================

inline uint16_t little_to_host_u16(uint16_t le_val) noexcept {
    return is_little_endian() ? le_val : swap_uint16(le_val);
}

inline uint32_t little_to_host_u32(uint32_t le_val) noexcept {
    return is_little_endian() ? le_val : swap_uint32(le_val);
}

inline uint64_t little_to_host_u64(uint64_t le_val) noexcept {
    return is_little_endian() ? le_val : swap_uint64(le_val);
}

inline float little_to_host_float(float le_val) noexcept {
    return is_little_endian() ? le_val : swap_float(le_val);
}

inline double little_to_host_double(double le_val) noexcept {
    return is_little_endian() ? le_val : swap_double(le_val);
}

// ============================================================
// 未对齐安全读取：从任意字节地址读小端标量并转主机序
// （NovAtel BIN 为小端；直接用 reinterpret_cast 在强对齐平台是 UB）
// ============================================================

inline uint16_t load_u16_le(const void* p) noexcept {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return little_to_host_u16(v);
}

inline int16_t load_i16_le(const void* p) noexcept {
    return static_cast<int16_t>(load_u16_le(p));
}

inline uint32_t load_u32_le(const void* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return little_to_host_u32(v);
}

inline float load_f32_le(const void* p) noexcept {
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(v));
    return little_to_host_float(v);
}

inline double load_f64_le(const void* p) noexcept {
    double v = 0.0;
    std::memcpy(&v, p, sizeof(v));
    return little_to_host_double(v);
}

} // namespace bin_io
} // namespace gnss
