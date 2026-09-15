/**
 * @file    bin_parser.cpp
 * @brief   NovAtel OEM7 BIN帧解析器核心实现
 * @details 完整的OEM7 BIN帧解析流程：
 *          1. 分块扫描 → 搜索同步头0xAA 0x44 0x12
 *          2. 解析OEM7帧头(标准28B，按 hdr_len 字段定位消息体)
 *             → 提取MsgID、GPS时间、消息体长度等
 *          3. 消息体解析 → 根据MsgID分发到对应解析函数
 *          4. CRC32校验 → 验证帧完整性
 *          5. 回调通知 → 将解析结果传递给上层
 *
 *          跨chunk帧处理：
 *          - 当chunk末尾残留不完整帧时，将剩余数据缓存到pending_buffer_
 *          - 下一chunk到来时，拼接pending数据与新chunk头部，继续解析
 */

#include "parser/bin_parser.h"
#include "parser/frame_header.h"
#include "bin_io/endian_utils.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <iostream>

namespace gnss {
namespace parser {

// ============================================================
// CRC32 查找表（IEEE 802.3 多项式）
// ============================================================

namespace {
    constexpr uint32_t CRC32_POLY = 0xEDB88320U;

    constexpr std::array<uint32_t, 256> make_crc32_table() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ ((crc & 1) ? CRC32_POLY : 0);
            }
            table[i] = crc;
        }
        return table;
    }

    constexpr auto CRC32_TABLE = make_crc32_table();

    /**
     * @brief 判定消息ID是否为"OEM7 官方已定义但本工具不解析"的日志
     * @note  这些帧 CRC 与长度均合法，只是不在支持范围内，故计入
     *        ParseStats::unsupported_frames 而非 malformed_frames。
     *        列表仅收录常用且确切已知的日志，可按需扩展。
     */
    constexpr bool is_known_unsupported_log(uint16_t msg_id) noexcept {
        switch (msg_id) {
            case MSG_ID_SATVIS:  // 48  ：OEM6 旧日志，OEM7 已由 SATVIS2 取代
            case 47:             // PSRPOS
            case 93:             // RXSTATUS
            case 99:             // BESTVEL
            case 101:            // TIME
            case 140:            // RANGECMP
            case 241:            // BESTXYZ
                return true;
            default:
                return false;
        }
    }
}

// ============================================================
// 同步头搜索与帧头解析
// ============================================================

size_t find_sync(const uint8_t* data, size_t len) noexcept {
    if (len < SYNC_LEN) return SIZE_MAX;

    for (size_t i = 0; i <= len - SYNC_LEN; ++i) {
        if (data[i] == SYNC_BYTE_0 &&
            data[i + 1] == SYNC_BYTE_1 &&
            data[i + 2] == SYNC_BYTE_2) {
            return i;
        }
    }
    return SIZE_MAX;
}

bool BinParser::verify_sync(const uint8_t* data) noexcept {
    return data[0] == SYNC_BYTE_0 &&
           data[1] == SYNC_BYTE_1 &&
           data[2] == SYNC_BYTE_2;
}

// ============================================================
// CRC32 计算
// ============================================================

uint32_t BinParser::calculate_crc32(const uint8_t* data, size_t length) noexcept {
    uint32_t crc = 0x00000000U;
    for (size_t i = 0; i < length; ++i) {
        uint8_t idx = static_cast<uint8_t>((crc ^ data[i]) & 0xFF);
        crc = (crc >> 8) ^ CRC32_TABLE[idx];
    }
    return crc ^ 0x00000000U;
}

void BinParser::log(const std::string& msg) {
    if (on_log_) {
        on_log_(msg);
    }
}

// ============================================================
// 主解析循环
// ============================================================

BinParser::ParseStats BinParser::parse(bin_io::MmapFile& mmap_file) {
    stats_.reset();

    std::vector<uint8_t> pending;
    pending.reserve(65536);

    size_t global_bytes_processed = 0;

    // SATVIS(48) 旧日志提示只打印一次，避免刷屏
    bool satvis_warned = false;

    do {
        const uint8_t* chunk_data = mmap_file.data();
        size_t chunk_len = mmap_file.current_chunk_size();

        if (chunk_data == nullptr || chunk_len == 0) {
            continue;
        }

        std::vector<uint8_t> scan_buffer;
        const uint8_t* scan_ptr = nullptr;
        size_t scan_len = 0;

        if (!pending.empty()) {
            scan_buffer.reserve(pending.size() + chunk_len);
            scan_buffer.assign(pending.begin(), pending.end());
            scan_buffer.insert(scan_buffer.end(), chunk_data, chunk_data + chunk_len);
            scan_ptr = scan_buffer.data();
            scan_len = scan_buffer.size();
            pending.clear();
        } else {
            scan_ptr = chunk_data;
            scan_len = chunk_len;
        }

        size_t pos = 0;

        while (pos + FRAME_MIN_LENGTH <= scan_len) {
            // 1. 搜索同步头
            size_t sync_offset = find_sync(scan_ptr + pos, scan_len - pos);
            if (sync_offset == SIZE_MAX) {
                if (pos > 0) {
                    stats_.sync_lost_count++;
                }
                // 整段已无同步头：丢弃垃圾数据，仅保留末尾 SYNC_LEN-1 字节，
                // 以防同步头恰好被分块边界截断（旧实现会把整段 tail 塞进 pending）
                const size_t keep = (scan_len < SYNC_LEN - 1) ? scan_len : (SYNC_LEN - 1);
                pending.assign(scan_ptr + scan_len - keep, scan_ptr + scan_len);
                break;
            }

            size_t frame_start = pos + sync_offset;

            if (sync_offset > 0) {
                stats_.sync_lost_count++;
            }

            if (frame_start + FRAME_MIN_LENGTH > scan_len) {
                pending.assign(scan_ptr + frame_start, scan_ptr + scan_len);
                break;
            }

            const uint8_t* frame_ptr = scan_ptr + frame_start;

            if (!verify_sync(frame_ptr)) {
                pos = frame_start + 1;
                continue;
            }

            // 解析OEM7帧头各字段（小端→主机序）
            // 官方 Binary 帧头长度可变（"Always check the header length"）：
            // 标准字段固定 28 字节，hdr_len >= 28，消息体起始 = frame_start + hdr_len
            size_t hdr_offset = SYNC_LEN;

            uint8_t hdr_len = frame_ptr[hdr_offset]; hdr_offset += 1;
            if (hdr_len < OEM7_HEADER_LENGTH) {
                stats_.malformed_frames++;
                pos = frame_start + 1;
                continue;
            }

            uint16_t msg_id   = bin_io::load_u16_le(frame_ptr + hdr_offset); hdr_offset += 2;
            uint8_t  msg_type = frame_ptr[hdr_offset]; (void)msg_type; hdr_offset += 1;
            uint8_t  port     = frame_ptr[hdr_offset]; (void)port;     hdr_offset += 1;
            uint16_t body_len = bin_io::load_u16_le(frame_ptr + hdr_offset); hdr_offset += 2;
            uint16_t seq      = bin_io::load_u16_le(frame_ptr + hdr_offset); (void)seq;      hdr_offset += 2;
            uint8_t  idle     = frame_ptr[hdr_offset]; (void)idle;     hdr_offset += 1;
            uint8_t  time_sts = frame_ptr[hdr_offset]; (void)time_sts; hdr_offset += 1;
            uint16_t week     = bin_io::load_u16_le(frame_ptr + hdr_offset); hdr_offset += 2;
            uint32_t tow_ms   = bin_io::load_u32_le(frame_ptr + hdr_offset); hdr_offset += 4;
            /* rcvr_status  */ hdr_offset += 4;
            /* reserved     */ hdr_offset += 2;
            /* sw_version   */ hdr_offset += 2;
            // hdr_offset 现在为 28（FRAME_HEADER_LENGTH，标准字段长度）；hdr_len 可能更大

            // 整帧长度必须按实际 hdr_len 计算：hdr_len + body_len + CRC32(4)
            size_t total_frame_len = static_cast<size_t>(hdr_len) + body_len + FRAME_CRC_LENGTH;

            if (frame_start + total_frame_len > scan_len) {
                // 帧尾尚未落入当前缓冲：缓存整个不完整帧，等下一块补齐
                pending.assign(frame_ptr, scan_ptr + scan_len);
                break;
            }

            // CRC32校验 (sync+header+body, 不含CRC本身；CRC为小端存放)
            size_t crc_coverage = static_cast<size_t>(hdr_len) + body_len;
            uint32_t computed_crc = calculate_crc32(frame_ptr, crc_coverage);

            const uint8_t* crc_ptr = frame_ptr + crc_coverage;
            uint32_t stored_crc = bin_io::load_u32_le(crc_ptr);

            if (computed_crc != stored_crc) {
                stats_.crc_error_count++;
                pos = frame_start + 1;
                continue;
            }

            GpsTime gps_time(week, tow_ms);
            const uint8_t* body_ptr = frame_ptr + hdr_len;  // 消息体起始按实际 hdr_len 定位

            bool parse_ok = false;

            switch (msg_id) {
                case MSG_ID_RANGE: {
                    // body: [观测数 uint32(4B)] + N × 44B
                    if (body_len < 4) {
                        stats_.malformed_frames++;
                        break;
                    }

                    constexpr size_t OBS_SIZE = 44;
                    const uint32_t num_obs = bin_io::load_u32_le(body_ptr);

                    // 不溢出形式：先校验余数，再校验观测数与长度自洽
                    if ((body_len - 4) % OBS_SIZE != 0 ||
                        num_obs != (body_len - 4) / OBS_SIZE) {
                        stats_.malformed_frames++;
                        break;
                    }

                    RangeFrame range_frame;
                    range_frame.gps_time = gps_time;
                    range_frame.num_observations = num_obs;
                    range_frame.observations.reserve(num_obs);

                    const uint8_t* p = body_ptr + 4;
                    for (uint32_t i = 0; i < num_obs; ++i) {
                        RangeObservation obs;
                        obs.sat_prn    = bin_io::load_u16_le(p);
                        obs.glofreq    = bin_io::load_u16_le(p + 2);
                        obs.pseudorange   = bin_io::load_f64_le(p + 4);
                        obs.psr_std       = bin_io::load_f32_le(p + 12);
                        obs.carrier_phase = bin_io::load_f64_le(p + 16);
                        obs.adr_std       = bin_io::load_f32_le(p + 24);
                        obs.doppler       = bin_io::load_f32_le(p + 28);
                        obs.cn0           = bin_io::load_f32_le(p + 32);
                        obs.locktime      = bin_io::load_f32_le(p + 36);
                        obs.ch_tr_status  = bin_io::load_u32_le(p + 40);

                        // 星座取 ch_tr_status 的 bit16-18（OEM7 Table 156），
                        // 不能用 bit21-25 的信号类型反推
                        obs.sat_system = system_from_ch_tr_status(obs.ch_tr_status);

                        range_frame.observations.push_back(obs);
                        p += OBS_SIZE;
                    }

                    if (on_range_) {
                        on_range_(range_frame);
                    }
                    stats_.range_frames++;
                    parse_ok = true;
                    break;
                }

                case MSG_ID_SATVIS: {
                    // OEM7 没有 SATVIS(48)（仅 OEM6 旧日志，布局与本工具原假设不符），
                    // 直接跳过，不再解析，避免真实数据被静默丢弃。
                    stats_.unsupported_frames++;
                    if (!satvis_warned) {
                        satvis_warned = true;
                        const char* msg =
                            "SATVIS(48) 是 OEM6 旧日志，OEM7 请用 SATVIS2(1043)，已跳过";
                        log(msg);
                        std::cerr << "[gnss_bin_parser] " << msg << std::endl;
                    }
                    break;
                }

                case MSG_ID_SATVIS2: {
                    // body: [System uint32][satVisValid uint32][almanacFlag uint32]
                    //       [numSats uint32] + N × 40B
                    if (body_len < 16) {
                        stats_.malformed_frames++;
                        break;
                    }

                    constexpr size_t SAT2_ENTRY_SIZE = 40;
                    if ((body_len - 16) % SAT2_ENTRY_SIZE != 0) {
                        stats_.malformed_frames++;
                        break;
                    }

                    const uint32_t num_sats = bin_io::load_u32_le(body_ptr + 12);
                    if (num_sats != (body_len - 16) / SAT2_ENTRY_SIZE) {
                        stats_.malformed_frames++;
                        break;
                    }

                    SatVis2Frame sv2_frame;
                    sv2_frame.gps_time = gps_time;
                    // 日志字段 Satellite System 用官方 Table 124 编号（0/1/2/5/6/7/9）
                    sv2_frame.sat_system = system_from_log_enum(bin_io::load_u32_le(body_ptr));
                    sv2_frame.sat_vis_valid = (bin_io::load_u32_le(body_ptr + 4) != 0);
                    sv2_frame.almanac_flag  = (bin_io::load_u32_le(body_ptr + 8) != 0);
                    sv2_frame.num_sats = num_sats;

                    const uint8_t* p = body_ptr + 16;
                    sv2_frame.sats.reserve(num_sats);
                    for (uint32_t i = 0; i < num_sats; ++i) {
                        SatVis2Entry sv;
                        const uint32_t sat_id = bin_io::load_u32_le(p);
                        sv.sat_prn   = static_cast<uint16_t>(sat_id & 0xFFFFU);
                        sv.glofreq   = static_cast<int16_t>((sat_id >> 16) & 0xFFFFU);
                        sv.health    = bin_io::load_u32_le(p + 4);
                        sv.elevation = bin_io::load_f64_le(p + 8);
                        sv.azimuth   = bin_io::load_f64_le(p + 16);
                        sv.true_doppler     = bin_io::load_f64_le(p + 24);
                        sv.apparent_doppler = bin_io::load_f64_le(p + 32);
                        sv2_frame.sats.push_back(sv);
                        p += SAT2_ENTRY_SIZE;
                    }

                    if (on_satvis2_) {
                        on_satvis2_(sv2_frame);
                    }
                    stats_.satvis2_frames++;
                    parse_ok = true;
                    break;
                }

                case MSG_ID_BESTPOS: {
                    constexpr size_t BESTPOSA_BODY_SIZE = 72;
                    if (body_len != BESTPOSA_BODY_SIZE) {
                        stats_.malformed_frames++;
                        break;
                    }

                    BestPosFrame bp_frame;
                    bp_frame.gps_time = gps_time;
                    bp_frame.solution_status = static_cast<SolutionStatus>(
                        bin_io::load_u32_le(body_ptr));
                    bp_frame.position_type = bin_io::load_u32_le(body_ptr + 4);
                    bp_frame.latitude  = bin_io::load_f64_le(body_ptr + 8);
                    bp_frame.longitude = bin_io::load_f64_le(body_ptr + 16);
                    bp_frame.height    = bin_io::load_f64_le(body_ptr + 24);
                    bp_frame.undulation = bin_io::load_f32_le(body_ptr + 32);
                    /* datum_id at +36 */
                    bp_frame.lat_std_dev = bin_io::load_f32_le(body_ptr + 40);
                    bp_frame.lon_std_dev = bin_io::load_f32_le(body_ptr + 44);
                    bp_frame.hgt_std_dev = bin_io::load_f32_le(body_ptr + 48);
                    bp_frame.num_svs      = body_ptr[64];
                    bp_frame.num_soln_svs = body_ptr[65];

                    if (on_bestpos_) {
                        on_bestpos_(bp_frame);
                    }
                    stats_.bestpos_frames++;
                    parse_ok = true;
                    break;
                }

                default: {
                    // 帧结构与 CRC 均合法，只是本工具不解析该日志
                    if (is_known_unsupported_log(msg_id)) {
                        stats_.unsupported_frames++;
                    } else {
                        stats_.malformed_frames++;
                    }
                    log("跳过未知消息ID=" + std::to_string(msg_id));
                    break;
                }
            }

            if (parse_ok) {
                stats_.total_frames++;
            }

            pos = frame_start + total_frame_len;
        }

        // 循环退出后仍可能剩下不足一帧最小长度的尾部：
        // 若其中含同步头，说明是跨块的残缺帧，保留；否则仅保留末尾 SYNC_LEN-1 字节
        if (pending.empty() && pos < scan_len) {
            size_t sync_off = find_sync(scan_ptr + pos, scan_len - pos);
            if (sync_off != SIZE_MAX) {
                pending.assign(scan_ptr + pos + sync_off, scan_ptr + scan_len);
            } else {
                const size_t remain = scan_len - pos;
                const size_t keep = (remain < SYNC_LEN - 1) ? remain : (SYNC_LEN - 1);
                pending.assign(scan_ptr + scan_len - keep, scan_ptr + scan_len);
            }
        }

        global_bytes_processed += chunk_len;
        stats_.bytes_processed = global_bytes_processed;

        if (on_progress_) {
            on_progress_(global_bytes_processed, mmap_file.file_size());
        }

    } while (mmap_file.next_chunk());

    if (!pending.empty() && pending.size() >= FRAME_MIN_LENGTH) {
        size_t sync_off = find_sync(pending.data(), pending.size());
        if (sync_off == SIZE_MAX) {
            stats_.sync_lost_count++;
        } else {
            // 文件末尾残留的疑似帧：若帧头不合法或整帧超界，计为结构性错误帧
            const uint8_t* frame_ptr = pending.data() + sync_off;
            const size_t avail = pending.size() - sync_off;
            if (avail < SYNC_LEN + OEM7_HEADER_LENGTH) {
                stats_.malformed_frames++;
            } else if (frame_ptr[SYNC_LEN] < OEM7_HEADER_LENGTH) {
                stats_.malformed_frames++;
            } else {
                const uint8_t  tail_hdr_len  = frame_ptr[SYNC_LEN];
                // body_len 位于 sync(3)+hdr_len(1)+msg_id(2)+msg_type(1)+port(1) = 偏移 8
                const uint16_t tail_body_len = bin_io::load_u16_le(frame_ptr + 8);
                if (avail < static_cast<size_t>(tail_hdr_len) + tail_body_len + FRAME_CRC_LENGTH) {
                    stats_.malformed_frames++;  // 帧被文件末尾截断
                }
            }
        }
    }

    return stats_;
}

// ============================================================
// FrameHeader 实现
// ============================================================

bool FrameHeader::is_valid() const noexcept {
    // 官方允许 hdr_len > 28（追加字段），故只要求不小于标准长度
    if (header_length < OEM7_HEADER_LENGTH) {
        return false;
    }
    if (message_id != MSG_ID_RANGE &&
        message_id != MSG_ID_SATVIS2 &&
        message_id != MSG_ID_BESTPOS) {
        return false;
    }
    if (body_length > 65500) {
        return false;
    }
    if (gps_time.tow_ms > 604800000) {
        return false;
    }
    return true;
}

const char* FrameHeader::message_name() const noexcept {
    switch (message_id) {
        case MSG_ID_RANGE:   return "RANGE观测(OEM7 ID=43)";
        case MSG_ID_SATVIS:  return "SATVIS(OEM6旧日志ID=48，OEM7不支持)";
        case MSG_ID_SATVIS2: return "SATVIS2卫星可见性(OEM7 ID=1043)";
        case MSG_ID_BESTPOS: return "BESTPOS定位结果(OEM7 ID=42)";
        default:             return "未知消息类型";
    }
}

} // namespace parser
} // namespace gnss
