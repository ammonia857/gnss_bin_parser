#pragma once
/**
 * @file    gnss_time.h
 * @brief   GNSS时间基类定义
 * @details 定义GPS时间结构体——GPS Week + 周内毫秒(TOW)。
 *          GPS Week 从1980年1月6日（GPS纪元）起算，是一个持续累加的整周数；
 *          本结构体只用 uint16_t 存储它（0~65535），硬件/协议层不做"满1024
 *          自动翻转"的截断，解析时也不做任何回绕还原。
 *          示例：GPS Week 2215 + 周内毫秒 代表某一具体观测时刻。
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
 *        示例：Week=2215, tow_ms=86400000 表示第2215周星期一00:00:00.000
 *              （86400000ms = 1天，因为GPS周从星期日00:00起算）
 */
struct GpsTime {
    uint16_t week = 0;    ///< GPS周编号（uint16_t存储，0~65535；不做1024周翻转处理）
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
 * @brief 卫星星座系统枚举（内部统一枚举）
 * @note  内部取值与任何一套官方编号都不完全一致，因此**禁止**把原始字段
 *        直接 static_cast 成本枚举；必须经由下面的显式映射函数转换：
 *        - ch-tr-status 的 bit16-18 → system_from_ch_tr_status()
 *        - 日志字段 Satellite System（OEM7 Table 124）→ system_from_log_enum()
 */
enum class SatelliteSystem : uint8_t {
    GPS = 0,      ///< 美国GPS系统
    BDS = 1,      ///< 中国北斗系统（BDS）
    GLONASS = 2,  ///< 俄罗斯GLONASS
    GALILEO = 3,  ///< 欧盟Galileo
    SBAS = 4,     ///< 星基增强系统
    QZSS = 5,     ///< 日本准天顶
    NAVIC = 6,    ///< 印度 NavIC（IRNSS）
    OTHER = 7,    ///< 其它系统
    UNKNOWN = 0xFF
};

/**
 * @brief 卫星系统转中文/通用名称
 * @param sys 卫星系统枚举
 * @return "GPS" / "北斗" / "GLONASS" / "Galileo" / "SBAS" / "QZSS" / "NavIC" / "其它" / "未知"
 */
[[nodiscard]] const char* system_to_name(SatelliteSystem sys) noexcept;

/**
 * @brief 卫星系统转单字符标识
 * @param sys 卫星系统枚举
 * @return 'G'/'C'/'R'/'E'/'S'/'J'/'I'(NavIC)/'?'(其它)
 */
[[nodiscard]] char system_to_char(SatelliteSystem sys) noexcept;

/**
 * @brief ch-tr-status 的 bit16-18（OEM7 Table 156 星座枚举）→ 内部枚举
 * @param ch_tr_status RANGE 观测值中的信道跟踪状态字原始值
 * @return 0=GPS,1=GLONASS,2=SBAS,3=Galileo,4=BeiDou,5=QZSS,6=NavIC,7=Other 对应的内部枚举
 * @note  bit21-25 是**信号类型**（含义依赖系统），不能用它反推星座。
 */
[[nodiscard]] SatelliteSystem system_from_ch_tr_status(uint32_t ch_tr_status) noexcept;

/**
 * @brief 日志字段 Satellite System（OEM7 Table 124：0/1/2/5/6/7/9）→ 内部枚举
 * @param value 日志中 4 字节 Satellite System 字段的原始值
 * @return 0=GPS,1=GLONASS,2=SBAS,5=Galileo,6=BeiDou,7=QZSS,9=NavIC；4/3/8 等空缺值 → UNKNOWN
 * @note  这套编号与 ch-tr-status 的星座编号**不是同一套**，不可混用。
 */
[[nodiscard]] SatelliteSystem system_from_log_enum(uint32_t value) noexcept;

} // namespace gnss
