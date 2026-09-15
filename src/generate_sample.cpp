/**
 * @file    generate_sample.cpp
 * @brief   NovAtel OEM7 BIN样本文件生成器（测试用）
 * @details 生成严格符合 OEM7 官方布局的测试文件，用于验证解析器正确性。
 *
 *          生成的日志：
 *          - RANGE(43)    ：每颗卫星的 ch-tr-status 按官方 Table 156 位域写入
 *                           （星座 bit16-18、信号类型 bit21-25），
 *                           使样本具备真正的"自证能力"，而不是把所有位都写 0。
 *          - SATVIS2(1043)：Satellite System 字段按官方 Table 124 编号写入
 *                           （0=GPS,1=GLO,2=SBAS,5=GAL,6=BDS,7=QZSS,9=NavIC），
 *                           每历元产出多个系统分组。
 *          - BESTPOS(42)  ：body 固定 72 字节，尾部字段按 OEM7 语义写入
 *                           （#SVs、#solnSVs、#solnL1SVs、#solnMultiSVs、
 *                            Reserved、ext sol stat、Galileo&BeiDou mask、
 *                            GPS&GLONASS mask）。
 *
 *          注：SATVIS(48) 是 OEM6 旧日志，OEM7 中已由 SATVIS2 取代，
 *              官方 OEM7 手册没有该日志，因此本生成器**不再产出** SATVIS(48)，
 *              以免解析器把它计入 unsupported_frames。
 *
 *          字节序：NovAtel BIN 全部多字节字段（含帧尾 CRC32）均为**小端**。
 *          CRC-32：poly=0xEDB88320、init=0、无最终异或、结果小端存放。
 *
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
// CRC32（与解析器相同的实现：poly 0xEDB88320 / init 0 / 无最终异或）
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

/**
 * @brief 写入一整帧：帧数据 + 小端 CRC32（覆盖 hdr + body）
 */
void write_frame(std::ofstream& os, const std::vector<uint8_t>& frame_data) {
    os.write(reinterpret_cast<const char*>(frame_data.data()),
             static_cast<std::streamsize>(frame_data.size()));

    uint32_t crc = calc_crc32(frame_data.data(), frame_data.size());

    // 显式小端存放，避免依赖宿主机字节序
    const uint8_t crc_le[4] = {
        static_cast<uint8_t>( crc        & 0xFFU),
        static_cast<uint8_t>((crc >>  8) & 0xFFU),
        static_cast<uint8_t>((crc >> 16) & 0xFFU),
        static_cast<uint8_t>((crc >> 24) & 0xFFU),
    };
    os.write(reinterpret_cast<const char*>(crc_le), 4);
}

// ---- 显式小端写入（不依赖宿主机字节序） ----

void put_u16_le(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>( v       & 0xFFU));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
}

void put_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>( v        & 0xFFU));
    buf.push_back(static_cast<uint8_t>((v >>  8) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
}

void put_u64_le(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFU));
    }
}

void put_float_le(std::vector<uint8_t>& buf, float v) {
    uint32_t bits = 0;
    static_assert(sizeof(float) == 4, "float 必须为 4 字节 IEEE754");
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32_le(buf, bits);
}

void put_double_le(std::vector<uint8_t>& buf, double v) {
    uint64_t bits = 0;
    static_assert(sizeof(double) == 8, "double 必须为 8 字节 IEEE754");
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64_le(buf, bits);
}

// ============================================================
// 官方编号常量
// ============================================================

namespace {

/// NovAtel OEM7 消息ID
constexpr uint16_t OEM7_ID_BESTPOS = 42;
constexpr uint16_t OEM7_ID_RANGE   = 43;
constexpr uint16_t OEM7_ID_SATVIS  = 48;    // OEM6 旧日志，本生成器不产出
constexpr uint16_t OEM7_ID_SATVIS2 = 1043;

/// RANGE ch-tr-status 中的星座编号（官方 Table 156）
enum OfficialChTrSystem : uint8_t {
    CHTR_GPS     = 0,
    CHTR_GLONASS = 1,
    CHTR_SBAS    = 2,
    CHTR_GALILEO = 3,
    CHTR_BDS     = 4,
    CHTR_QZSS    = 5,
    CHTR_NAVIC   = 6,
    CHTR_OTHER   = 7,
};

/// 日志字段 Satellite System 编号（官方 Table 124）
enum OfficialLogSystem : uint8_t {
    LOG_SYS_GPS     = 0,
    LOG_SYS_GLONASS = 1,
    LOG_SYS_SBAS    = 2,
    LOG_SYS_GALILEO = 5,
    LOG_SYS_BDS     = 6,
    LOG_SYS_QZSS    = 7,
    LOG_SYS_NAVIC   = 9,
};

/**
 * @brief 按官方位域组装 ch-tr-status
 * @param official_system 官方星座号（Table 156）→ bit16-18
 * @param signal_type     信号类型号（含义依赖系统）→ bit21-25
 */
constexpr uint32_t make_ch_tr_status(uint8_t official_system, uint8_t signal_type) {
    return (static_cast<uint32_t>(official_system & 0x07U) << 16) |
           (static_cast<uint32_t>(signal_type    & 0x1FU) << 21);
}

/**
 * @brief RANGE 样本中的一颗卫星（一个频点的观测）
 */
struct RangeSatSpec {
    uint16_t    prn;              ///< 卫星PRN
    uint8_t     official_system;  ///< 官方 Table 156 星座号
    uint8_t     signal_type;      ///< ch-tr-status bit21-25 信号类型
    int         glofreq;          ///< NovAtel 语义 = GLONASS Frequency + 7（非GLO为0）
    float       doppler_hz;       ///< 多普勒（Hz）
    float       cn0;              ///< 载噪比（dB-Hz）
    const char* label;            ///< 说明（仅打印用）
};

/**
 * @brief SATVIS2 的一个卫星系统分组
 */
struct SatVis2Group {
    uint8_t     log_system;   ///< 官方 Table 124 系统号
    uint16_t    first_prn;    ///< 该组第一颗卫星PRN
    uint8_t     count;        ///< 该组卫星数
    double      doppler_hz;   ///< 理论/视多普勒基准（Hz）
    const char* label;        ///< 说明（仅打印用）
};

} // namespace

/**
 * @brief 生成OEM7帧头（28字节）
 * @details OEM7帧头格式：
 *   Sync(3B) + HdrLen(1B=28) + MsgID(2B) + MsgType(1B=0) + Port(1B=0)
 *   + MsgLen(2B) + Seq(2B) + Idle(1B) + TimeSts(1B)
 *   + Week(2B) + GPSms(4B) + RcvStatus(4B) + Reserved(2B) + SWVer(2B)
 *   所有多字节字段小端存放。
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
                      << "  -n <num>   生成的历元数量\n\n"
                      << "OEM7帧格式: 28字节帧头 + 消息体 + CRC32（全部小端）\n"
                      << "  BESTPOS ID=42, RANGE ID=43, SATVIS2 ID=1043\n"
                      << "  不生成 SATVIS(48)：该日志为 OEM6 旧日志，OEM7 无此日志\n"
                      << "RANGE ch-tr-status 按官方 Table 156 位域写入；\n"
                      << "SATVIS2 系统字段按官方 Table 124 写入。\n";
            return 0;
        }
    }

    if (num_epochs <= 0) {
        std::cerr << "错误: -n 必须为正整数" << std::endl;
        return 1;
    }

    std::ofstream ofs(output_file, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "无法创建文件: " << output_file << std::endl;
        return 1;
    }

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> pr_dist(20000000.0, 37000000.0);
    std::uniform_real_distribution<double> cp_dist(0.0, 1e8);
    // 允许负仰角（官方示例中甚至出现过 -82.3°）
    std::uniform_real_distribution<float>  el_dist(-25.0f, 90.0f);
    std::uniform_real_distribution<float>  az_dist(0.0f, 360.0f);
    std::uniform_real_distribution<double> lat_jitter(-0.001, 0.001);
    std::uniform_real_distribution<double> lon_jitter(-0.001, 0.001);

    const double BASE_LAT = 39.9042;
    const double BASE_LON = 116.4074;
    const double BASE_HGT = 45.0;      // MSL 高程（OEM7 的 hgt 字段语义）
    const double UNDULATION = -9.4;    // 大地水准面差距（m）

    // ============================================================
    // RANGE 观测样本：每历元覆盖 GPS L1C/A、GPS L2C(M)、GLONASS L1C/A、
    // Galileo E5a、BDS B1I、BDS B2a、SBAS、QZSS
    //   —— 其中 GPS L2C(M) = sys0/sig17 是关键回归用例：
    //      旧实现误用 bit21-25 反推星座，会把它判成 BDS。
    // ============================================================
    const std::vector<RangeSatSpec> range_sats = {
        {   5, CHTR_GPS,     0,  0, 2450.0f, 45.0f, "GPS L1C/A"    },
        {   5, CHTR_GPS,    17,  0, 1910.0f, 43.5f, "GPS L2C(M)"   },
        {  12, CHTR_GLONASS, 0,  8, 1600.0f, 44.0f, "GLO L1C/A"    },
        {   3, CHTR_GALILEO,12,  0, 1750.0f, 42.0f, "GAL E5a"      },
        {   7, CHTR_BDS,     0,  0, 2100.0f, 46.0f, "BDS B1I"      },
        {   7, CHTR_BDS,     9,  0, 1530.0f, 44.5f, "BDS B2a"      },
        { 120, CHTR_SBAS,    0,  0, 1200.0f, 38.0f, "SBAS L1C/A"   },
        { 194, CHTR_QZSS,    0,  0, 2500.0f, 41.0f, "QZSS L1C/A"   },
    };

    // ============================================================
    // SATVIS2 系统分组：官方 Table 124 编号（GPS/GLO/SBAS/GAL/BDS）
    // ============================================================
    const std::vector<SatVis2Group> satvis2_groups = {
        { LOG_SYS_GPS,      1,  8, 2450.0, "GPS"     },
        { LOG_SYS_GLONASS,  8,  6, 1600.0, "GLONASS" },
        { LOG_SYS_SBAS,   120,  5, 1200.0, "SBAS"    },
        { LOG_SYS_GALILEO,  1,  6, 1750.0, "Galileo" },
        { LOG_SYS_BDS,      1, 10, 2100.0, "BeiDou"  },
    };

    const uint16_t GPS_WEEK = 2215;
    constexpr uint8_t BESTPOS_BODY_LEN = 72;

    const size_t frames_per_epoch = 1 + satvis2_groups.size() + 1;  // RANGE + SATVIS2 + BESTPOS

    std::cout << "生成NovAtel OEM7 BIN样本文件: " << output_file << std::endl;
    std::cout << "历元数量: " << num_epochs << std::endl;
    std::cout << "GPS Week: " << GPS_WEEK << std::endl;
    std::cout << "每历元帧数: " << frames_per_epoch
              << " (1 RANGE + " << satvis2_groups.size() << " SATVIS2 + 1 BESTPOS)"
              << std::endl;

    for (int epoch = 0; epoch < num_epochs; ++epoch) {
        uint32_t tow_ms = static_cast<uint32_t>(epoch) * 30000U;
        uint16_t seq = static_cast<uint16_t>(epoch);

        // ============================================================
        // 1. RANGE 观测帧 (OEM7 ID=43, 44字节/观测)
        // ============================================================
        {
            const uint32_t num_obs = static_cast<uint32_t>(range_sats.size());
            uint16_t body_len = static_cast<uint16_t>(4 + num_obs * 44);

            std::vector<uint8_t> body;
            put_u32_le(body, num_obs);

            for (const auto& sat : range_sats) {
                put_u16_le(body, sat.prn);                     // prn
                put_u16_le(body, static_cast<uint16_t>(sat.glofreq)); // glofreq
                put_double_le(body, pr_dist(rng));             // psr
                put_float_le(body, 0.5f);                      // psr_std
                put_double_le(body, cp_dist(rng));             // adr
                put_float_le(body, 0.01f);                     // adr_std
                put_float_le(body, sat.doppler_hz);            // dopp
                put_float_le(body, sat.cn0);                   // cn0
                put_float_le(body, 10.0f);                     // locktime
                put_u32_le(body, make_ch_tr_status(sat.official_system,
                                                   sat.signal_type)); // ch_tr_status
            }

            auto hdr = make_oem7_header(OEM7_ID_RANGE, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // ============================================================
        // 2. SATVIS2 卫星可见性扩展帧 (OEM7 ID=1043)
        //    格式: 16B header(system/valid/almanac/numSats) + N*40B entries
        //    系统字段用官方 Table 124 编号
        // ============================================================
        for (const auto& grp : satvis2_groups) {
            constexpr size_t SAT2_ENTRY_SIZE = 40;
            const uint32_t num_sats = grp.count;
            uint16_t body_len = static_cast<uint16_t>(16 + num_sats * SAT2_ENTRY_SIZE);

            std::vector<uint8_t> body;
            put_u32_le(body, static_cast<uint32_t>(grp.log_system)); // 官方 Table 124
            put_u32_le(body, 1);                    // sat_vis_valid
            put_u32_le(body, 0);                    // almanac_flag
            put_u32_le(body, num_sats);

            for (uint32_t i = 0; i < num_sats; ++i) {
                const uint16_t prn = static_cast<uint16_t>(grp.first_prn + i);
                // sat_id: 低16位 PRN，高16位 GLONASS 频率通道（非GLO为0）
                const int16_t glofreq = (grp.log_system == LOG_SYS_GLONASS)
                                        ? static_cast<int16_t>(7 + static_cast<int>(i % 14))
                                        : 0;
                const uint32_t sat_id = static_cast<uint32_t>(prn) |
                                        (static_cast<uint32_t>(static_cast<uint16_t>(glofreq)) << 16);

                put_u32_le(body, sat_id);
                put_u32_le(body, 0);                            // health
                put_double_le(body, static_cast<double>(el_dist(rng)));
                put_double_le(body, static_cast<double>(az_dist(rng)));
                put_double_le(body, grp.doppler_hz);            // true_doppler
                put_double_le(body, grp.doppler_hz + 1.5);      // apparent_doppler
            }

            auto hdr = make_oem7_header(OEM7_ID_SATVIS2, GPS_WEEK, tow_ms, body_len, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        // 注：SATVIS(48) 为 OEM6 旧日志，OEM7 无此日志，故不生成。

        // ============================================================
        // 3. BESTPOS 定位结果帧 (OEM7 ID=42, body 72字节)
        //    偏移 64..71 按 OEM7 语义：
        //      64 #SVs(tracked) / 65 #solnSVs(used) / 66 #solnL1SVs
        //      67 #solnMultiSVs / 68 Reserved / 69 ext sol stat
        //      70 Galileo&BeiDou sig mask / 71 GPS&GLONASS sig mask
        //    Height_m 为 MSL 高程（椭球高 = Height_m + Undulation_m）
        // ============================================================
        {
            double lat = BASE_LAT + lat_jitter(rng);
            double lon = BASE_LON + lon_jitter(rng);
            double hgt = BASE_HGT + lat_jitter(rng) * 10.0;   // MSL 高程

            std::vector<uint8_t> body;
            put_u32_le(body, static_cast<uint32_t>(SolutionStatus::SOL_COMPUTED));
            put_u32_le(body, epoch < num_epochs / 2 ? 16 : 50); // pos_type: SINGLE or NARROW_INT
            put_double_le(body, lat);
            put_double_le(body, lon);
            put_double_le(body, hgt);
            put_float_le(body, static_cast<float>(UNDULATION)); // undulation
            put_u32_le(body, 61);          // datum_id = WGS84
            put_float_le(body, 1.5f);      // lat_std_dev
            put_float_le(body, 1.2f);      // lon_std_dev
            put_float_le(body, 2.8f);      // hgt_std_dev
            put_u32_le(body, 0);           // stn_id
            put_float_le(body, 0.0f);      // diff_age
            put_float_le(body, 0.0f);      // sol_age

            // --- 偏移 64..71：OEM7 尾部字段 ---
            body.push_back(14);            // 64: #SVs           (tracked)
            body.push_back(12);            // 65: #solnSVs       (used)
            body.push_back(12);            // 66: #solnL1SVs
            body.push_back(10);            // 67: #solnMultiSVs
            body.push_back(0x00);          // 68: Reserved
            body.push_back(0x00);          // 69: ext sol stat
            body.push_back(0x08);          // 70: Galileo and BeiDou sig mask
            body.push_back(0x0F);          // 71: GPS and GLONASS sig mask

            if (body.size() != BESTPOS_BODY_LEN) {
                std::cerr << "内部错误: BESTPOS body 长度 " << body.size()
                          << " != " << static_cast<int>(BESTPOS_BODY_LEN) << std::endl;
                return 1;
            }

            auto hdr = make_oem7_header(OEM7_ID_BESTPOS, GPS_WEEK, tow_ms,
                                        BESTPOS_BODY_LEN, seq);
            hdr.insert(hdr.end(), body.begin(), body.end());
            write_frame(ofs, hdr);
        }

        if ((epoch + 1) % 10 == 0 || epoch == num_epochs - 1) {
            std::cout << "  已生成 " << (epoch + 1) << " / " << num_epochs
                      << " 个历元" << std::endl;
        }
    }

    ofs.close();
    if (ofs.fail()) {
        std::cerr << "警告: 写入流状态异常，文件可能不完整: " << output_file << std::endl;
        return 1;
    }

    std::cout << "\nOEM7样本文件生成完成: " << output_file << std::endl;
    std::cout << "  帧总数: " << (static_cast<size_t>(num_epochs) * frames_per_epoch) << " ("
              << num_epochs << " RANGE + "
              << (static_cast<size_t>(num_epochs) * satvis2_groups.size()) << " SATVIS2 + "
              << num_epochs << " BESTPOS)"
              << std::endl;
    std::cout << "  说明: 不生成 SATVIS(48)（OEM6 旧日志，OEM7 无此日志）" << std::endl;

    return 0;
}
