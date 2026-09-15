#pragma once
/**
 * @file    frame_header.h
 * @brief   NovAtel OEM7 BIN帧统一帧头定义
 * @details 定义OEM7 BIN文件中每条消息帧的通用帧头结构。
 *          每条帧 = 帧头(hdr_len字节) + 消息体(变长) + CRC32(4字节)
 *
 *          OEM7帧头格式（小端字节序存储；帧尾CRC亦为小端）：
 *          ┌───────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
 *          │ Sync  │Hdr Len   │Msg ID    │Msg Type  │Port Addr │Msg Len   │Sequence  │
 *          │ 3B    │ 1B (≥28) │ 2B       │ 1B       │ 1B       │ 2B       │ 2B       │
 *          ├───────┼──────────┼──────────┼──────────┼──────────┼──────────┼──────────┤
 *          │Idle   │Time Sts  │GPS Week  │GPS ms            │Rcv Status│Reserved  │
 *          │ 1B    │ 1B       │ 2B       │ 4B               │ 4B       │ 2B       │
 *          ├───────┴──────────┴──────────┴──────────────────┴──────────┴──────────┤
 *          │Rcv SW Ver │
 *          │ 2B         │
 *          └────────────┘
 *
 *          同步头: 0xAA 0x44 0x12  (3字节，NovAtel OEM7标准)
 *          帧头长度: 标准字段 28 字节，官方允许追加字段，故解析时必须
 *                    "always check the header length"（hdr_len >= 28），
 *                    消息体起始偏移 = frame_start + hdr_len。
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

/// 帧头标准字段长度（字节）：28。官方允许追加字段，实际长度见 hdr_len
constexpr size_t   FRAME_HEADER_LENGTH = 28;

/// 帧尾CRC32长度（字节，小端存放）
constexpr size_t   FRAME_CRC_LENGTH = 4;

/// 帧最小长度：标准帧头(28) + CRC(4) = 32（用于分块扫描的缓冲下限）
constexpr size_t   FRAME_MIN_LENGTH = FRAME_HEADER_LENGTH + FRAME_CRC_LENGTH;

/// OEM7帧头标准字段长度（hdr_len 的最小合法值；实际取帧头中的 hdr_len 字段）
constexpr uint8_t  OEM7_HEADER_LENGTH = 28;

// ============================================================
// NovAtel OEM7 标准消息ID定义
// ============================================================

/// BESTPOS 定位结果帧消息ID（NovAtel OEM7: BESTPOSA/B）
constexpr uint16_t MSG_ID_BESTPOS = 42;

/// RANGE 观测日志帧消息ID（NovAtel OEM7: RANGEA/B）
constexpr uint16_t MSG_ID_RANGE   = 43;

/// SATVIS 消息ID：OEM6 旧日志，OEM7 已由 SATVIS2(1043) 取代；本工具不支持解析
constexpr uint16_t MSG_ID_SATVIS  = 48;

/// SATVIS2 卫星可见性扩展帧消息ID（NovAtel OEM7）
constexpr uint16_t MSG_ID_SATVIS2 = 1043;

// 官方"消息类型"字节语义：bit0-4 = 测量源，bit5-6 = 格式
//   00 = Binary, 01 = ASCII, 10 = Abbreviated ASCII, 11 = NMEA
/// 消息类型字节中的"格式"位掩码（bit5-6）
constexpr uint8_t  MSG_TYPE_FORMAT_MASK   = 0x60;
/// 消息类型字节的"格式"字段取值：Binary
constexpr uint8_t  MSG_TYPE_FORMAT_BINARY = 0x00;

/**
 * @brief OEM7 BIN帧统一帧头结构体（解析后，主机字节序）
 * @note  从OEM7 BIN二进制帧头解析得到，所有多字节字段已完成小端→主机序转换。
 */
struct FrameHeader {
    // --- OEM7标准字段 ---
    uint8_t  header_length = 0;   ///< 帧头长度 hdr_len（标准28，可能因追加字段更大）
    uint16_t message_id = 0;      ///< 消息类型ID（42=BESTPOS/43=RANGE/1043=SATVIS2）
    uint8_t  message_type = 0;    ///< 消息类型字节（bit5-6格式：0=Binary/1=ASCII/2=Abbrev ASCII/3=NMEA）
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
     * @return true=帧头长度不小于标准长度、消息ID已知且body_length合理
     */
    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief 获取整帧总长度（帧头 + 消息体 + CRC）
     * @return 整帧字节数（以实际 hdr_len 计算）
     */
    [[nodiscard]] size_t total_frame_length() const noexcept {
        return static_cast<size_t>(header_length) + body_length + FRAME_CRC_LENGTH;
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
