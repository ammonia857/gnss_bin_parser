/**
 * @file    generate_sample.cpp
 * @brief   NovAtel OEM7 BIN样本文件生成器（测试用）
 * @details 生成符合OEM7 BIN格式规范的测试文件，包含RANGE(ID=43)、SATVIS(ID=48)、BESTPOS(ID=42)三类帧。
 *          用于验证解析器的正确性。
 *          用法: gnss_gen_sample -o sample.bin -n 100
 */

#include "bin_io/endian_utils.h"
#include "gnss_struct/gnss_time.h"
#include "gnss_struct/gnss_bands.h"
#include "gnss_struct/range_obs.h"
#include "gnss_struct/sat_vis.h"
#include "gnss_struct/best_pos.h"

#include <iostream>
#include <fstream>
#include <array>
#include <vector>
#include <cstring>
#include <string>
#include <random>
#include <ctime>

using namespace gnss;
using namespace gnss::bin_io;

// ============================================================
// CRC32（与解析器相同的实现）
// ============================================================

namespace {
    constexpr uint32_t CRC32_POLY = 0xEDB88320U;

    constexpr auto make_crc32_table() {
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

    uint32_t calc_crc32(const uint8_t* data, size_t len) {
        uint32_t crc = 0x00000000U;
        for (size_t i = 0; i < len; ++i) {
            uint8_t idx = (crc ^ data[i]) & 0xFF;
            crc = (crc >> 8) ^ CRC32_TABLE[idx];
        }
        return crc ^ 0x00000000U;
    }
}

void write_frame(std::ofstream& os, const std::vector<uint8_t>& frame_data) {
    os.write(reinterpret_cast<const char*>(frame_data.data()), frame_data.size());

    uint32_t crc = calc_crc32(frame_data.data(), frame_data.size());
    os.write(reinterpret_cast<const char*>(&crc), 4);
}

void put_u16_le(std::vector<uint8_t>& buf, uint16_t val) {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&val),
               reinterpret_cast<const uint8_t*>(&val) + 2);
}

void put_u32_le(std::vector<uint8_t>& buf, uint32_t val) {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&val),
               reinterpret_cast<const uint8_t*>(&val) + 4);
}

void put_double_le(std::vector<uint8_t>& buf, double val) {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&val),
               reinterpret_cast<const uint8_t*>(&val) + 8);
}

void put_float_le(std::vector<uint8_t>& buf, float val) {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&val),
               reinterpret_cast<const uint8_t*>(&val) + 4);
}

// NovAtel OEM7 消息ID
constexpr uint16_t OEM7_ID_BESTPOS = 42;
constexpr uint16_t OEM7_ID_RANGE   = 43;
constexpr uint16_t OEM7_ID_SATVIS  = 48;
constexpr uint16_t OEM7_ID_SATVIS2 = 1043;

/**
 * @brief 生成OEM7帧头（28字节）
 * @details OEM7帧头格式：
 *   Sync(3B) + HdrLen(1B=28) + MsgID(2B) + MsgType(1B=0) + Port(1B=0)
 *   + MsgLen(2B) + Seq(2B) + Idle(1B) + TimeSts(1B)
 *   + Week(2B) + GPSms(4B) + RcvStatus(4B) + Reserved(2B) + SWVer(2B)
 */
std::vector<uint8_t> make_oem7_header(uint16_t msg_id, uint16_t week, uint32_t tow_ms,
                                       uint16_t body_len, uint16_t sequence) {
    std::vector<uint8_t> hdr;

    // Sync (3B)
    hdr.push_back(0xAA);
    hdr.push_back(0x44);
    hdr.push_back(0x12);

    // Header Length (1B) = 28
    hdr.push_back(28);

    // Message ID (2B, little-endian)
    put_u16_le(hdr, msg_id);

    // Message Type (1B) = 0 (Binary)
    hdr.push_back(0);

    // Port Address (1B)
    hdr.push_back(0);

    // Message Length (2B, little-endian) — body only
    put_u16_le(hdr, body_len);

    // Sequence (2B, little-endian)
    put_u16_le(hdr, sequence);

    // Idle Time (1B)
    hdr.push_back(0);

    // Time Status (1B) — 160 = FINESTEERING
    hdr.push_back(160);

    // GPS Week (2B, little-endian)
    put_u16_le(hdr, week);

    // GPS Milliseconds (4B, little-endian)
    put_u32_le(hdr, tow_ms);

    // Receiver Status (4B)
    put_u32_le(hdr, 0x00000000);

    // Reserved (2B)
    put_u16_le(hdr, 0);

    // Receiver SW Version (2B)
    put_u16_le(hdr, 0x0700); // 7.00

    return hdr;
}

int main(int argc, char* argv[]) {
    std::string output_file = "sample.bin";
    int num_epochs = 10;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "-n" && i + 1 < argc) {
            num_epochs = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "NovAtel OEM7 BIN样本生成器\n"
                      << "用法: " << argv[0] << " -o sample.bin -n 100\n"
                      << "  -o <file>  输出文件路径\n"
                      << "  -n <num>   生成的历元数量（每个历元含3类帧）\n\n"
                      << "OEM7帧格式: 28字节帧头 + 消息体 + CRC32\n"
                      << "  BESTPOS ID=42, RANGE ID=43, SATVIS ID=48\n";
            return 0;
        }
    }

    std::ofstream ofs(output_file, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "无法创建文件: " << output_file << std::endl;
        return 1;
    }

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> pr_dist(20000000.0, 37000000.0);
    std::uniform_real_distribution<double> cp_dist(0.0, 1e8);
    std::uniform_real_distribution<float>  el_dist(3.0f, 90.0f);
    std::uniform_real_distribution<float>  az_dist(0.0f, 360.0f);
    std::uniform_real_distribution<double> lat_jitter(-0.001, 0.001);
    std::uniform_real_distribution<double> lon_jitter(-0.001, 0.001);

    const double BASE_LAT = 39.9042;
    const double BASE_LON = 116.4074;
    const double BASE_HGT = 45.0;

    const std::vector<uint8_t> gps_prns = {1, 3, 6, 8, 10, 14, 17, 22, 26, 30};
    const std::vector<uint8_t> bds_prns = {1, 2, 3, 5, 7, 9, 12, 16, 19, 24, 28, 33};

    const uint16_t GPS_WEEK = 2215;

    std::cout << "生成NovAtel OEM7 BIN样本文件: " << output_file << std::endl;
    std::cout << "历元数量: " << num_epochs << std::endl;
    std::cout << "GPS Week: " << GPS_WEEK << std::endl;

    for (int epoch = 0; epoch < num_epochs; ++epoch) {
        uint32_t tow_ms = epoch * 30000;
        uint16_t seq = static_cast<uint16_t>(epoch);

        // ============================================================
        // 1. RANGE 观测帧 (OEM7 ID=43, 44字节obs格式)
        // ============================================================
        {
            uint32_t num_obs = static_cast<uint32_t>(gps_prns.size() + bds_prns.size());
            uint16_t body_len = static_cast<uint16_t>(4 + num_obs * 44);

            std::vector<uint8_t> body;
            put_u32_le(body, num_obs);

            for (uint8_t prn : gps_prns) {
                put_u16_le(body, prn);         // prn
                put_u16_le(body, 0);           // glofreq
                put_double_le(body, pr_dist(rng)); // psr
                put_float_le(body, 0.5f);      // psr_std
                put_double_le(body, cp_dist(rng)); // adr
                put_float_le(body, 0.01f);     // adr_std
                put_float_le(body, 2450.0f);   // dopp (L1 ~2450 Hz)
                put_float_le(body, 42.0f);     // cn0
                put_float_le(body, 10.0f);     // locktime
                put_u32_le(body, 0);           // ch_tr_status
            }

            for (uint8_t prn : bds_prns) {
                put_u16_le(body, prn);
                put_u16_le(body, 0);
                put_double_le(body, pr_dist(rng));
                put_float_le(body, 0.5f);
                put_double_le(body, cp_dist(rng));
                put_float_le(body, 0.01f);
                put_float_le(body, 2100.0f);   // dopp (B1I ~2100 Hz)
                put_float_le(body, 44.0f);     // cn0
                put_float_le(body, 10.0f);
                put_u32_le(body, 0);
            }

            auto hdr = make_oem7_header(OEM7_ID_RANGE, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 2. SATVIS 卫星可见性帧 - GPS (OEM7 ID=48)
        // ============================================================
        {
            uint8_t total_sats = static_cast<uint8_t>(gps_prns.size());
            uint16_t body_len = 4 + total_sats * 10;

            std::vector<uint8_t> body;
            body.push_back(static_cast<uint8_t>(SatelliteSystem::GPS));
            body.push_back(total_sats);
            body.push_back(0);
            body.push_back(0);

            for (uint8_t prn : gps_prns) {
                body.push_back(prn);
                body.push_back(0);
                put_float_le(body, el_dist(rng));
                put_float_le(body, az_dist(rng));
            }

            auto hdr = make_oem7_header(OEM7_ID_SATVIS, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 3. SATVIS 卫星可见性帧 - BDS (OEM7 ID=48)
        // ============================================================
        {
            uint8_t total_sats = static_cast<uint8_t>(bds_prns.size());
            uint16_t body_len = 4 + total_sats * 10;

            std::vector<uint8_t> body;
            body.push_back(static_cast<uint8_t>(SatelliteSystem::BDS));
            body.push_back(total_sats);
            body.push_back(0);
            body.push_back(0);

            for (uint8_t prn : bds_prns) {
                body.push_back(prn);
                body.push_back(0);
                put_float_le(body, el_dist(rng));
                put_float_le(body, az_dist(rng));
            }

            auto hdr = make_oem7_header(OEM7_ID_SATVIS, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 4. SATVIS2 卫星可见性扩展帧 - GPS (OEM7 ID=1043)
        //    格式: 16B header + N*40B entries
        // ============================================================
        {
            uint32_t num_sats = static_cast<uint32_t>(gps_prns.size());
            constexpr size_t SAT2_ENTRY_SIZE = 40;
            uint16_t body_len = static_cast<uint16_t>(16 + num_sats * SAT2_ENTRY_SIZE);

            std::vector<uint8_t> body;
            put_u32_le(body, static_cast<uint32_t>(SatelliteSystem::GPS));
            put_u32_le(body, 1);                    // sat_vis_valid
            put_u32_le(body, 0);                    // almanac_flag
            put_u32_le(body, num_sats);

            for (uint8_t prn : gps_prns) {
                put_u32_le(body, prn);              // sat_id (PRN in low 16 bits)
                put_u32_le(body, 0);                // health
                put_double_le(body, static_cast<double>(el_dist(rng)));
                put_double_le(body, static_cast<double>(az_dist(rng)));
                put_double_le(body, 2450.0);        // true_doppler (Hz)
                put_double_le(body, 2450.0);        // apparent_doppler (Hz)
            }

            auto hdr = make_oem7_header(OEM7_ID_SATVIS2, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 5. SATVIS2 卫星可见性扩展帧 - BDS
        // ============================================================
        {
            uint32_t num_sats = static_cast<uint32_t>(bds_prns.size());
            constexpr size_t SAT2_ENTRY_SIZE = 40;
            uint16_t body_len = static_cast<uint16_t>(16 + num_sats * SAT2_ENTRY_SIZE);

            std::vector<uint8_t> body;
            put_u32_le(body, static_cast<uint32_t>(SatelliteSystem::BDS));
            put_u32_le(body, 1);                    // sat_vis_valid
            put_u32_le(body, 0);                    // almanac_flag
            put_u32_le(body, num_sats);

            for (uint8_t prn : bds_prns) {
                put_u32_le(body, prn);              // sat_id (PRN in low 16 bits)
                put_u32_le(body, 0);                // health
                put_double_le(body, static_cast<double>(el_dist(rng)));
                put_double_le(body, static_cast<double>(az_dist(rng)));
                put_double_le(body, 2100.0);        // true_doppler (Hz, B1I)
                put_double_le(body, 2100.0);        // apparent_doppler (Hz)
            }

            auto hdr = make_oem7_header(OEM7_ID_SATVIS2, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 6. BESTPOSA 定位结果帧 (OEM7 ID=42, 72字节)
        // ============================================================
        {
            constexpr uint16_t BODY_LEN = 72;

            double lat = BASE_LAT + lat_jitter(rng);
            double lon = BASE_LON + lon_jitter(rng);
            double hgt = BASE_HGT + lat_jitter(rng) * 10.0;

            std::vector<uint8_t> body;
            put_u32_le(body, static_cast<uint32_t>(SolutionStatus::SOL_COMPUTED));
            put_u32_le(body, epoch < num_epochs / 2 ? 16 : 50); // pos_type: SINGLE or NARROW_INT
            put_double_le(body, lat);
            put_double_le(body, lon);
            put_double_le(body, hgt);
            put_float_le(body, 0.0f);     // undulation
            put_u32_le(body, 0);           // datum_id
            put_float_le(body, 1.5f);      // lat_std_dev
            put_float_le(body, 1.2f);      // lon_std_dev
            put_float_le(body, 2.8f);      // hgt_std_dev
            put_u32_le(body, 0);           // stn_id
            put_float_le(body, 0.0f);      // diff_age
            put_float_le(body, 0.0f);      // sol_age
            // num_svs, num_soln_svs, etc. (8 bytes of counters)
            body.push_back(10);  // num_svs
            body.push_back(8);   // num_soln_svs
            body.push_back(5);   // num_gg_l1
            body.push_back(5);   // num_gg_l1_l2
            body.push_back(0);   // num_gg_l1_l5
            body.push_back(0);   // reserved1
            body.push_back(0);   // num_glo_l1
            body.push_back(0);   // num_glo_l2

            auto hdr = make_oem7_header(OEM7_ID_BESTPOS, GPS_WEEK, tow_ms, BODY_LEN, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        if ((epoch + 1) % 10 == 0 || epoch == num_epochs - 1) {
            std::cout << "  已生成 " << (epoch + 1) << " / " << num_epochs
                      << " 个历元" << std::endl;
        }
    }

    ofs.close();
    std::cout << "\nOEM7样本文件生成完成: " << output_file << std::endl;
    std::cout << "  帧总数: " << (num_epochs * 6) << " ("
              << num_epochs << " RANGE + "
              << (num_epochs * 2) << " SATVIS + "
              << (num_epochs * 2) << " SATVIS2 + "
              << num_epochs << " BESTPOS)" << std::endl;

    return 0;
}
