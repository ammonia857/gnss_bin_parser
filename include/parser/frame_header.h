#pragma once
/**
 * @file    frame_header.h
 * @brief   NovAtel OEM7 BIN帧统一帧头定义
 * @details 定义OEM7 BIN文件中每条消息帧的通用帧头结构。
 *          每条帧 = 帧头(28字节) + 消息体(变长) + CRC32(4字节)
 *
 *          OEM7帧头格式（大端字节序存储）：
 *          ┌───────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
 *          │ Sync  │Hdr Len   │Msg ID    │Msg Type  │Port Addr │Msg Len   │Sequence  │
 *          │ 3B    │ 1B (=28) │ 2B       │ 1B       │ 1B       │ 2B       │ 2B       │
 *          ├───────┼──────────┼──────────┼──────────┼──────────┼──────────┼──────────┤
 *          │Idle   │Time Sts  │GPS Week  │GPS ms            │Rcv Status│Reserved  │
 *          │ 1B    │ 1B       │ 2B       │ 4B               │ 4B       │ 2B       │
 *          ├───────┴──────────┴──────────┴──────────────────┴──────────┴──────────┤
 *          │Rcv SW Ver │
 *          │ 2B         │
 *          └────────────┘
 *
 *          同步头: 0xAA 0x44 0x12  (3字节，NovAtel OEM7标准)
 *          帧头长度: 固定28字节
 */

#include <cstdint>
#include "gnss_struct/gnss_time.h"

namespace gnss {
namespace parser {

/// OEM7 BIN帧同步字节序列（NovAtel标准）
constexpr uint8_t  SYNC_BYTE_0 = 0xAA;
constexpr uint8_t  SYNC_BYTE_1 = 0x44;
constexpr uint8_t  SYNC_BYTE_2 = 0x12;
constexpr size_t   SYNC_LEN = 3;

/// OEM7帧头固定长度（字节）：28
constexpr size_t   FRAME_HEADER_LENGTH = 28;

/// 帧尾CRC32长度（字节）
constexpr size_t   FRAME_CRC_LENGTH = 4;

/// 帧最小长度：帧头(28) + CRC(4) = 32
constexpr size_t   FRAME_MIN_LENGTH = FRAME_HEADER_LENGTH + FRAME_CRC_LENGTH;

/// OEM7帧头长度固定值
constexpr uint8_t  OEM7_HEADER_LENGTH = 28;

// ============================================================
// NovAtel OEM7 标准消息ID定义
// ============================================================

/// BESTPOS 定位结果帧消息ID（NovAtel OEM7: BESTPOSA/B）
constexpr uint16_t MSG_ID_BESTPOS = 42;

/// RANGE 观测日志帧消息ID（NovAtel OEM7: RANGEA/B）
constexpr uint16_t MSG_ID_RANGE   = 43;

/// SATVIS 卫星可见性帧消息ID（NovAtel OEM7: SATVIS/SATVIS2）
constexpr uint16_t MSG_ID_SATVIS  = 48;

/// SATVIS2 卫星可见性扩展帧消息ID（NovAtel OEM7）
constexpr uint16_t MSG_ID_SATVIS2 = 1043;

/// 消息类型：二进制
constexpr uint8_t  MSG_TYPE_BINARY = 0;

/**
 * @brief OEM7 BIN帧统一帧头结构体（解析后，主机字节序）
 * @note  从OEM7 BIN二进制帧头解析得到，所有多字节字段已完成大端→主机序转换。
 */
struct FrameHeader {
    // --- OEM7标准字段 ---
    uint8_t  header_length = 0;   ///< 帧头长度（OEM7固定28）
    uint16_t message_id = 0;      ///< 消息类型ID（42=BESTPOS/43=RANGE/48=SATVIS）
    uint8_t  message_type = 0;    ///< 消息类型（0=Binary, 1=ASCII, 2=Abbrev. ASCII, 3=NMEA）
    uint8_t  port_address = 0;    ///< 端口地址
    uint16_t body_length = 0;     ///< 消息体长度（字节，不含帧头和CRC）
    uint16_t sequence = 0;        ///< 序列号（用于检测丢帧）
    uint8_t  idle_time = 0;       ///< 空闲时间（0.5秒单位）
    uint8_t  time_status = 0;     ///< 时间状态
    GpsTime  gps_time;            ///< GPS观测时刻（week + tow_ms）
    uint32_t receiver_status = 0; ///< 接收机状态字
    uint16_t reserved = 0;        ///< 保留字段
    uint16_t receiver_sw_version = 0; ///< 接收机软件版本号

    FrameHeader() = default;

    /**
     * @brief 检查帧头是否有效
     * @return true=消息ID已知且body_length合理
     */
    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief 获取整帧总长度（帧头+消息体+CRC）
     * @return 整帧字节数
     */
    [[nodiscard]] size_t total_frame_length() const noexcept {
        return FRAME_HEADER_LENGTH + body_length + FRAME_CRC_LENGTH;
    }

    /** @brief 获取消息类型名称 */
    [[nodiscard]] const char* message_name() const noexcept;
};

/**
 * @brief 在字节缓冲区中搜索下一个OEM7同步头位置
 * @param data 数据起始指针
 * @param len  数据长度
 * @return 同步头偏移（0~len-3），未找到返回 SIZE_MAX
 */
size_t find_sync(const uint8_t* data, size_t len) noexcept;

} // namespace parser
} // namespace gnss
