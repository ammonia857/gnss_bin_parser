/**
 * @file    bin_parser.cpp
 * @brief   NovAtel OEM7 BIN帧解析器核心实现
 * @details 完整的OEM7 BIN帧解析流程：
 *          1. 分块扫描 → 搜索同步头0xAA 0x44 0x12
 *          2. 解析OEM7帧头(28B) → 提取MsgID、GPS时间、消息体长度等
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

bool BinParser::parse_header(bin_io::BinStreamReader& reader, FrameHeader& header) {
    // OEM7帧头(28字节): Sync(3)已跳过
    // HdrLen(1)+MsgID(2)+MsgType(1)+Port(1)+MsgLen(2)+Seq(2)+Idle(1)+TimeSts(1)
    // +Week(2)+GPSms(4)+RcvStatus(4)+Reserved(2)+SWVer(2) = 25字节(不含sync)

    header.header_length = reader.read_u8();

    if (header.header_length != OEM7_HEADER_LENGTH) {
        return false;
    }

    header.message_id   = reader.read_u16();
    header.message_type = reader.read_u8();
    header.port_address = reader.read_u8();
    header.body_length  = reader.read_u16();
    header.sequence     = reader.read_u16();
    header.idle_time    = reader.read_u8();
    header.time_status  = reader.read_u8();
    header.gps_time.week   = reader.read_u16();
    header.gps_time.tow_ms = reader.read_u32();
    header.receiver_status     = reader.read_u32();
    header.reserved            = reader.read_u16();
    header.receiver_sw_version = reader.read_u16();

    return true;
}

// ============================================================
// 消息体解析函数
// ============================================================

bool BinParser::parse_range_body(bin_io::BinStreamReader& reader,
                                  uint16_t num_obs, RangeFrame& frame) {
    (void)num_obs;
    (void)frame;
    // RANGE body parsing is handled inline in BinParser::parse()
    // for 44-byte observation format via direct memory access.
    return false;
}

bool BinParser::parse_satvis_body(bin_io::BinStreamReader& reader,
                                   uint8_t total_sats, SatVisFrame& frame) {
    frame.total_sats = total_sats;
    frame.sats.clear();
    frame.sats.reserve(total_sats);

    for (uint8_t i = 0; i < total_sats; ++i) {
        SatVisibility sv;

        sv.sat_prn   = reader.read_u8();
        reader.skip(1); // reserved
        sv.elevation = reader.read_float();
        sv.azimuth   = reader.read_float();

        if (sv.elevation < -5.0f || sv.elevation > 95.0f ||
            sv.azimuth < 0.0f   || sv.azimuth > 360.0f) {
            return false;
        }

        frame.sats.push_back(sv);
    }

    return true;
}

bool BinParser::parse_bestpos_body(bin_io::BinStreamReader& reader,
                                    BestPosFrame& frame) {
    (void)reader;
    (void)frame;
    // BESTPOS body parsing is handled inline in BinParser::parse()
    // for 72-byte BESTPOSA format via direct memory access.
    return false;
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

    bin_io::BinStreamReader reader(mmap_file);

    std::vector<uint8_t> pending;
    pending.reserve(65536);

    size_t global_bytes_processed = 0;

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
                size_t tail_start = pos;
                size_t tail_len = scan_len - tail_start;
                if (tail_len > 0 && tail_len < FRAME_MIN_LENGTH) {
                    pending.assign(scan_ptr + tail_start, scan_ptr + scan_len);
                }
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

            // 解析OEM7帧头(28字节)各字段（大端→主机序）
            size_t hdr_offset = SYNC_LEN;

            uint8_t hdr_len = frame_ptr[hdr_offset]; hdr_offset += 1;
            if (hdr_len != OEM7_HEADER_LENGTH) {
                pos = frame_start + 1;
                continue;
            }

            uint16_t msg_id   = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(frame_ptr + hdr_offset)); hdr_offset += 2;
            uint8_t  msg_type = frame_ptr[hdr_offset]; (void)msg_type; hdr_offset += 1;
            uint8_t  port     = frame_ptr[hdr_offset]; (void)port;     hdr_offset += 1;
            uint16_t body_len = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(frame_ptr + hdr_offset)); hdr_offset += 2;
            uint16_t seq      = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(frame_ptr + hdr_offset)); (void)seq;      hdr_offset += 2;
            uint8_t  idle     = frame_ptr[hdr_offset]; (void)idle;     hdr_offset += 1;
            uint8_t  time_sts = frame_ptr[hdr_offset]; (void)time_sts; hdr_offset += 1;
            uint16_t week     = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(frame_ptr + hdr_offset)); hdr_offset += 2;
            uint32_t tow_ms   = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(frame_ptr + hdr_offset)); hdr_offset += 4;
            /* rcvr_status  */ hdr_offset += 4;
            /* reserved     */ hdr_offset += 2;
            /* sw_version   */ hdr_offset += 2;
            // hdr_offset should now be 28 (FRAME_HEADER_LENGTH)

            if (body_len > 65535 - FRAME_HEADER_LENGTH - FRAME_CRC_LENGTH) {
                pos = frame_start + 1;
                continue;
            }

            size_t total_frame_len = FRAME_HEADER_LENGTH + body_len + FRAME_CRC_LENGTH;

            if (frame_start + total_frame_len > scan_len) {
                pending.assign(frame_ptr, scan_ptr + scan_len);
                break;
            }

            // CRC32校验 (sync+header+body, 不含CRC本身)
            size_t crc_coverage = FRAME_HEADER_LENGTH + body_len;
            uint32_t computed_crc = calculate_crc32(frame_ptr, crc_coverage);

            const uint8_t* crc_ptr = frame_ptr + crc_coverage;
            uint32_t stored_crc = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(crc_ptr));

            if (computed_crc != stored_crc) {
                stats_.crc_error_count++;
                pos = frame_start + 1;
                continue;
            }

            GpsTime gps_time(week, tow_ms);
            const uint8_t* body_ptr = frame_ptr + FRAME_HEADER_LENGTH;

            bool parse_ok = false;

            switch (msg_id) {
                case MSG_ID_RANGE: {
                    if (body_len < 4) break;

                    uint32_t num_obs = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(body_ptr));

                    constexpr size_t OBS_SIZE = 44;
                    if (body_len != 4 + num_obs * OBS_SIZE) break;

                    RangeFrame range_frame;
                    range_frame.gps_time = gps_time;
                    range_frame.num_observations = num_obs;
                    range_frame.observations.reserve(num_obs);

                    const uint8_t* p = body_ptr + 4;
                    for (uint32_t i = 0; i < num_obs; ++i) {
                        RangeObservation obs;
                        obs.sat_prn    = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(p));
                        obs.glofreq    = bin_io::little_to_host_u16(*reinterpret_cast<const uint16_t*>(p + 2));
                        obs.pseudorange   = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 4));
                        obs.psr_std       = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 12));
                        obs.carrier_phase = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 16));
                        obs.adr_std       = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 24));
                        obs.doppler       = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 28));
                        obs.cn0           = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 32));
                        obs.locktime      = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 36));
                        obs.ch_tr_status  = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(p + 40));

                        // 从ch_tr_status的bit21-25提取信号类型，推导卫星系统
                        uint32_t sig_type = (obs.ch_tr_status >> 21) & 0x1F;
                        if (sig_type <= 14)
                            obs.sat_system = SatelliteSystem::GPS;
                        else if (sig_type >= 17 && sig_type <= 30)
                            obs.sat_system = SatelliteSystem::BDS;
                        // else stays UNKNOWN

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
                    if (body_len < 4) break;

                    SatVisFrame sv_frame;
                    sv_frame.gps_time = gps_time;
                    sv_frame.sat_system = static_cast<SatelliteSystem>(body_ptr[0]);
                    sv_frame.total_sats  = body_ptr[1];

                    constexpr size_t SAT_SIZE = 10;
                    if (body_len != 4 + static_cast<size_t>(sv_frame.total_sats) * SAT_SIZE) break;

                    const uint8_t* p = body_ptr + 4;
                    sv_frame.sats.reserve(sv_frame.total_sats);
                    for (uint8_t i = 0; i < sv_frame.total_sats; ++i) {
                        SatVisibility sv;
                        sv.sat_prn   = p[0];
                        sv.elevation = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 2));
                        sv.azimuth   = bin_io::little_to_host_float(*reinterpret_cast<const float*>(p + 6));
                        sv_frame.sats.push_back(sv);
                        p += SAT_SIZE;
                    }

                    if (on_satvis_) {
                        on_satvis_(sv_frame);
                    }
                    stats_.satvis_frames++;
                    parse_ok = true;
                    break;
                }

                case MSG_ID_SATVIS2: {
                    if (body_len < 16) break;

                    SatVis2Frame sv2_frame;
                    sv2_frame.gps_time = gps_time;
                    sv2_frame.sat_system = static_cast<SatelliteSystem>(
                        bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(body_ptr)));
                    sv2_frame.sat_vis_valid = (body_ptr[4] != 0 || body_ptr[5] != 0 ||
                                               body_ptr[6] != 0 || body_ptr[7] != 0);
                    sv2_frame.almanac_flag  = (body_ptr[8] != 0 || body_ptr[9] != 0 ||
                                               body_ptr[10] != 0 || body_ptr[11] != 0);
                    sv2_frame.num_sats = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(body_ptr + 12));

                    constexpr size_t SAT2_ENTRY_SIZE = 40;
                    size_t expected_body = 16 + sv2_frame.num_sats * SAT2_ENTRY_SIZE;
                    if (body_len != expected_body) break;

                    const uint8_t* p = body_ptr + 16;
                    sv2_frame.sats.reserve(sv2_frame.num_sats);
                    for (uint32_t i = 0; i < sv2_frame.num_sats; ++i) {
                        SatVis2Entry sv;
                        uint32_t sat_id = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(p));
                        sv.sat_prn   = static_cast<uint16_t>(sat_id & 0xFFFF);
                        sv.glofreq   = static_cast<int16_t>((sat_id >> 16) & 0xFFFF);
                        sv.health    = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(p + 4));
                        sv.elevation = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 8));
                        sv.azimuth   = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 16));
                        sv.true_doppler     = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 24));
                        sv.apparent_doppler = bin_io::little_to_host_double(*reinterpret_cast<const double*>(p + 32));
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
                    if (body_len != BESTPOSA_BODY_SIZE) break;

                    BestPosFrame bp_frame;
                    bp_frame.gps_time = gps_time;
                    bp_frame.solution_status = static_cast<SolutionStatus>(
                        bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(body_ptr)));
                    bp_frame.position_type = bin_io::little_to_host_u32(*reinterpret_cast<const uint32_t*>(body_ptr + 4));
                    bp_frame.latitude  = bin_io::little_to_host_double(*reinterpret_cast<const double*>(body_ptr + 8));
                    bp_frame.longitude = bin_io::little_to_host_double(*reinterpret_cast<const double*>(body_ptr + 16));
                    bp_frame.height    = bin_io::little_to_host_double(*reinterpret_cast<const double*>(body_ptr + 24));
                    bp_frame.undulation = bin_io::little_to_host_float(*reinterpret_cast<const float*>(body_ptr + 32));
                    /* datum_id at +36 */
                    bp_frame.lat_std_dev = bin_io::little_to_host_float(*reinterpret_cast<const float*>(body_ptr + 40));
                    bp_frame.lon_std_dev = bin_io::little_to_host_float(*reinterpret_cast<const float*>(body_ptr + 44));
                    bp_frame.hgt_std_dev = bin_io::little_to_host_float(*reinterpret_cast<const float*>(body_ptr + 48));
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
                    log("跳过未知消息ID=" + std::to_string(msg_id));
                    break;
                }
            }

            if (parse_ok) {
                stats_.total_frames++;
            }

            pos = frame_start + total_frame_len;
        }

        if (pos < scan_len && pending.empty()) {
            size_t remain = scan_len - pos;
            if (remain < FRAME_MIN_LENGTH) {
                pending.assign(scan_ptr + pos, scan_ptr + scan_len);
            }
            if (remain >= FRAME_MIN_LENGTH) {
                pending.assign(scan_ptr + pos, scan_ptr + scan_len);
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
        }
    }

    return stats_;
}

// ============================================================
// FrameHeader 实现
// ============================================================

bool FrameHeader::is_valid() const noexcept {
    if (header_length != OEM7_HEADER_LENGTH) {
        return false;
    }
    if (message_id != MSG_ID_RANGE &&
        message_id != MSG_ID_SATVIS &&
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
        case MSG_ID_SATVIS:  return "SATVIS卫星可见性(OEM7 ID=48)";
        case MSG_ID_SATVIS2: return "SATVIS2卫星可见性(OEM7 ID=1043)";
        case MSG_ID_BESTPOS: return "BESTPOS定位结果(OEM7 ID=42)";
        default:             return "未知消息类型";
    }
}

} // namespace parser
} // namespace gnss
