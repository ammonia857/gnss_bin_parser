#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NovAtel OEM7 BIN 解析工具 —— 防回归测试（Python 3，仅用标准库）

用途
----
用**官方算法**（CRC-32: poly=0xEDB88320, init=0, 无最终异或, 结果小端存放）
自行构造 OEM7 BIN 帧，交给被测可执行文件 `gnss_parser` 解析，然后校验 CSV 输出，
确保以下历史缺陷不会回归：

  1. 官方手册 BESTPOSB 示例帧必须能正确解析出 lat/lon/undulation/卫星数
     （同时用官方文档给出的 CRC `42 dc 4c 48` 验证本脚本的 CRC 实现与官方一致）。
  2. RANGE 星座必须取 ch-tr-status 的 bit16-18（官方 Table 156），
     尤其 GPS L2C(M)（sys=0, sig=17）必须是 GPS 而不是被误判成北斗——
     这是旧实现按 bit21-25 反推星座时的经典错判。
  3. SATVIS2 的系统字段必须按官方 Table 124（0/1/2/5/6/7/9）映射到内部枚举
     （0,2,4,3,1,5,6），且 Sat_System_Name 列非空且与之一致。
  4. SATVIS(48) 是 OEM6 旧日志，必须不被解析，且计入 unsupported_frames。
  5. CRC 被改坏的帧必须计入 crc_error，且其数据不得出现在 CSV 中。
  6. CSV 惰性建文件：某类数据没有时不得创建对应 CSV 文件。

用法
----
    python tests/regression.py                     # 自动推导 build/Release/gnss_parser.exe
    python tests/regression.py --exe path/to/exe   # 指定被测可执行文件
    python tests/regression.py --keep              # 保留临时输出目录以便排查

退出码：全部 PASS 时为 0；任一断言失败时为 1（并打印清晰的期望/实际 diff）。
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

# ============================================================
# 官方 CRC-32（poly 0xEDB88320 / init 0 / 无最终异或）
# ============================================================

CRC32_POLY = 0xEDB88320

_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (CRC32_POLY if (_c & 1) else 0)
    _CRC_TABLE.append(_c)


def crc32(data: bytes) -> int:
    """与 NovAtel 官方 CalculateBlockCRC32 完全一致的 CRC-32 实现。"""
    crc = 0
    for b in data:
        crc = (crc >> 8) ^ _CRC_TABLE[(crc ^ b) & 0xFF]
    return crc


# ============================================================
# 小端打包辅助
# ============================================================

def u16(v): return struct.pack('<H', v & 0xFFFF)
def u32(v): return struct.pack('<I', v & 0xFFFFFFFF)
def i16(v): return struct.pack('<h', v)
def f32(v): return struct.pack('<f', v)
def f64(v): return struct.pack('<d', v)


# ============================================================
# 官方编号
# ============================================================

MSG_ID_BESTPOS = 42
MSG_ID_RANGE = 43
MSG_ID_SATVIS = 48
MSG_ID_SATVIS2 = 1043

# 内部枚举（CSV Sat_System 列）
INT_GPS, INT_BDS, INT_GLO, INT_GAL, INT_SBAS, INT_QZSS, INT_NAVIC = 0, 1, 2, 3, 4, 5, 6

SYSTEM_NAME = {
    0: 'GPS',
    1: '北斗',
    2: 'GLONASS',
    3: 'Galileo',
    4: 'SBAS',
    5: 'QZSS',
    6: 'NavIC',
    7: '其它',
    255: '未知',
}


def ch_tr_status(official_system: int, signal_type: int) -> int:
    """按官方位域组装 ch-tr-status：星座 bit16-18、信号类型 bit21-25。"""
    return ((official_system & 0x07) << 16) | ((signal_type & 0x1F) << 21)


# ============================================================
# 帧构造
# ============================================================

HDR_LEN = 28


def make_frame(msg_id, body, week=2215, tow_ms=0, seq=0, port=0x20,
               time_status=160, hdr_len=HDR_LEN, crc_override=None):
    """构造一条完整 OEM7 BIN 帧：帧头 + body + 小端 CRC32。"""
    hdr = bytearray()
    hdr += b'\xAA\x44\x12'
    hdr.append(hdr_len)
    hdr += u16(msg_id)
    hdr.append(0)          # message type: bit5-6=0 → Binary
    hdr.append(port)
    hdr += u16(len(body))
    hdr += u16(seq)
    hdr.append(0)          # idle time
    hdr.append(time_status)
    hdr += u16(week)
    hdr += u32(tow_ms)
    hdr += u32(0)          # receiver status
    hdr += u16(0)          # reserved
    hdr += u16(0x0700)     # receiver SW version
    assert len(hdr) == hdr_len, (len(hdr), hdr_len)

    payload = bytes(hdr) + body
    crc = crc32(payload) if crc_override is None else crc_override
    return payload + u32(crc)


def range_body(obs_list):
    """obs_list: [(prn, ch_tr_status, glofreq, psr, adr, dopp, cn0), ...]"""
    body = u32(len(obs_list))
    for (prn, cts, glofreq, psr, adr, dopp, cn0) in obs_list:
        body += u16(prn)
        body += u16(glofreq)
        body += f64(psr)
        body += f32(0.5)      # psr_std
        body += f64(adr)
        body += f32(0.01)     # adr_std
        body += f32(dopp)
        body += f32(cn0)
        body += f32(10.0)     # locktime
        body += u32(cts)
    return body


def satvis2_body(log_system, sats):
    """sats: [(prn, glofreq, health, el, az, true_dopp, app_dopp), ...]
    log_system 使用官方 Table 124 编号。"""
    body = u32(log_system) + u32(1) + u32(0) + u32(len(sats))
    for (prn, glofreq, health, el, az, td, ad) in sats:
        sat_id = (prn & 0xFFFF) | ((glofreq & 0xFFFF) << 16)
        body += u32(sat_id)
        body += u32(health)
        body += f64(el)
        body += f64(az)
        body += f64(td)
        body += f64(ad)
    return body


def satvis48_body(system_byte, sats):
    """OEM6 旧日志 SATVIS(48) 的（本项目不再支持的）布局，内容任意。"""
    body = bytearray()
    body.append(system_byte & 0xFF)
    body.append(len(sats) & 0xFF)
    body.append(0)
    body.append(0)
    for (prn, el, az) in sats:
        body.append(prn & 0xFF)
        body.append(0)
        body += f32(el)
        body += f32(az)
    return bytes(body)


# ---- 官方手册 BESTPOSB 示例帧（NovAtel OEM7 文档 "32-bit CRC" 页给出的原始字节） ----

OFFICIAL_BESTPOSB = bytes([
    0xAA, 0x44, 0x12, 0x1C, 0x2A, 0x00, 0x02, 0x20, 0x48, 0x00,
    0x00, 0x00, 0x90, 0xB4, 0x93, 0x05, 0xB0, 0xAB, 0xB9, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x45, 0x61, 0xBC, 0x0A, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x1B, 0x04, 0x50, 0xB3,
    0xF2, 0x8E, 0x49, 0x40, 0x16, 0xFA, 0x6B, 0xBE, 0x7C, 0x82,
    0x5C, 0xC0, 0x00, 0x60, 0x76, 0x9F, 0x44, 0x9F, 0x90, 0x40,
    0xA6, 0x2A, 0x82, 0xC1, 0x3D, 0x00, 0x00, 0x00, 0x12, 0x5A,
    0xCB, 0x3F, 0xCD, 0x9E, 0x98, 0x3F, 0xDB, 0x66, 0x40, 0x40,
    0x00, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x06, 0x00, 0x03,
])
OFFICIAL_BESTPOSB_CRC = bytes([0x42, 0xDC, 0x4C, 0x48])

# 官方示例帧的字段值（由上面的字节解码得到）
# 注：同一文档页的 ASCII 示例给的是另一个历元（1427, 325298.000），
#     与本二进制示例并非同一条记录；此处以**二进制示例字节**为准。
OFFICIAL_WEEK = 1427         # 帧头 week 字段 (0x0593)
OFFICIAL_TOW_MS = 314158000  # 帧头 ms 字段 (0x12B9ABB0)
OFFICIAL_LAT = 51.11678162962945
OFFICIAL_LON = -114.03886375946635
OFFICIAL_HGT = 1063.8170145507902
OFFICIAL_UNDULATION = -16.270824432373047
OFFICIAL_NSVS = 11          # #SVs   (tracked)
OFFICIAL_NSOLN_SVS = 11     # #solnSVs (used)


# ============================================================
# CSV 读取
# ============================================================

def read_csv(path):
    """返回 (header_list, [dict, ...])；自动去除 UTF-8 BOM。"""
    with open(path, 'rb') as f:
        raw = f.read()
    text = raw.decode('utf-8-sig')
    lines = [ln for ln in text.splitlines() if ln.strip() != '']
    if not lines:
        return [], []
    header = lines[0].split(',')
    rows = []
    for ln in lines[1:]:
        cells = ln.split(',')
        if len(cells) != len(header):
            raise AssertionError(
                '列数不一致: %s\n  表头(%d列): %s\n  数据行(%d列): %s'
                % (path, len(header), header, len(cells), cells))
        rows.append(dict(zip(header, cells)))
    return header, rows


# ============================================================
# 测试框架
# ============================================================

class Failures(object):
    def __init__(self):
        self.items = []

    def check(self, cond, label, detail=''):
        if cond:
            print('    [ok]   %s' % label)
        else:
            print('    [FAIL] %s%s' % (label, ('\n           ' + detail) if detail else ''))
            self.items.append(label if not detail else '%s | %s' % (label, detail))

    def eq(self, actual, expected, label):
        self.check(actual == expected, label,
                   'expected=%r\n           actual  =%r' % (expected, actual))

    def close(self, actual, expected, tol, label):
        try:
            ok = abs(float(actual) - float(expected)) <= tol
        except (TypeError, ValueError):
            ok = False
        self.check(ok, label,
                   'expected=%.10g (±%g)\n           actual  =%r' % (expected, tol, actual))


def find_default_exe(script_path):
    root = os.path.dirname(os.path.dirname(os.path.abspath(script_path)))
    candidates = [
        os.path.join(root, 'build', 'Release', 'gnss_parser.exe'),
        os.path.join(root, 'build', 'gnss_parser.exe'),
        os.path.join(root, 'build', 'Release', 'gnss_parser'),
        os.path.join(root, 'build', 'gnss_parser'),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def run_parser(exe, bin_path, out_dir):
    """运行被测程序，返回 (returncode, stdout, stderr)。"""
    proc = subprocess.run(
        [exe, '-i', bin_path, '-o', out_dir],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        encoding='utf-8', errors='replace')
    return proc.returncode, proc.stdout or '', proc.stderr or ''


def stat_value(stdout, token):
    """从统计输出中抓取 `xxx (token):  N` 形式的计数。"""
    m = re.search(re.escape(token) + r'\s*\)\s*[:：]\s*(\d+)', stdout)
    return int(m.group(1)) if m else None


def csv_path(out_dir, case_name, kind):
    return os.path.join(out_dir, 'gnss_%s_%s.csv' % (case_name, kind))


def list_csvs(out_dir):
    if not os.path.isdir(out_dir):
        return []
    return sorted(n for n in os.listdir(out_dir) if n.endswith('.csv'))


def write_bin(out_dir, case_name, blob):
    path = os.path.join(out_dir, case_name + '.bin')
    with open(path, 'wb') as f:
        f.write(blob)
    return path


# ============================================================
# 各项测试
# ============================================================

def test_00_official_crc_selftest(fx):
    """自检：本脚本的 CRC 实现必须复现官方文档给出的 42 dc 4c 48。"""
    calc = crc32(OFFICIAL_BESTPOSB)
    fx.eq('%02x %02x %02x %02x' % tuple(calc.to_bytes(4, 'little')),
          '42 dc 4c 48',
          '自检: 官方 BESTPOSB 示例帧 CRC（本脚本算法 == 官方算法）')
    fx.eq(len(OFFICIAL_BESTPOSB), 28 + 72, '自检: 官方示例帧长度 = 28(帧头) + 72(body)')


def test_01_official_bestposb(exe, out_dir, fx):
    """官方手册 BESTPOSB 示例帧解析。"""
    print('\n[case 1] 官方手册 BESTPOSB 示例帧（官方字节 + 官方 CRC 42 dc 4c 48）')
    case = 'official_bestpos'
    bin_path = write_bin(out_dir, case, OFFICIAL_BESTPOSB + OFFICIAL_BESTPOSB_CRC)

    rc, out, err = run_parser(exe, bin_path, out_dir)
    fx.eq(rc, 0, '退出码 = 0（解析成功）')

    path = csv_path(out_dir, case, 'bestpos')
    fx.check(os.path.isfile(path), 'bestpos.csv 已创建（惰性建文件：有数据才创建）')
    if not os.path.isfile(path):
        return
    header, rows = read_csv(path)
    fx.eq(','.join(header),
          'GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,Longitude_deg,'
          'Height_m,Undulation_m,LatStd_m,LonStd_m,HgtStd_m,SVs_Tracked,SVs_Used',
          'BESTPOS 表头（SVs_Tracked 在 SVs_Used 之前，与写入顺序一致）')
    fx.eq(len(rows), 1, 'bestpos 数据行数 = 1')
    if not rows:
        return
    r = rows[0]
    fx.eq(r['GPS_Week'], str(OFFICIAL_WEEK), 'GPS_Week = 1427（帧头 0x0593）')
    fx.eq(r['TOW_ms'], str(OFFICIAL_TOW_MS), 'TOW_ms = 314158000（帧头 0x12B9ABB0）')
    fx.eq(r['Solution_Status'], '0', 'Solution_Status = 0 (SOL_COMPUTED)')
    fx.eq(r['Position_Type'], '16', 'Position_Type = 16 (SINGLE)')
    fx.close(r['Latitude_deg'], OFFICIAL_LAT, 1e-8, 'Latitude_deg ≈ 51.11678…')
    fx.close(r['Longitude_deg'], OFFICIAL_LON, 1e-8, 'Longitude_deg ≈ -114.03886…')
    fx.close(r['Height_m'], OFFICIAL_HGT, 1e-3, 'Height_m ≈ 1063.817（MSL 高程）')
    fx.close(r['Undulation_m'], OFFICIAL_UNDULATION, 1e-4, 'Undulation_m ≈ -16.2708')
    fx.eq(r['SVs_Tracked'], str(OFFICIAL_NSVS), 'SVs_Tracked = 11 (#SVs @ body+64)')
    fx.eq(r['SVs_Used'], str(OFFICIAL_NSOLN_SVS), 'SVs_Used = 11 (#solnSVs @ body+65)')
    fx.eq(list_csvs(out_dir).count('gnss_%s_range.csv' % case), 0,
          '未解析到 RANGE → 不创建 range.csv（惰性建文件）')


def test_02_range_systems(exe, out_dir, fx):
    """RANGE 星座取 ch-tr-status bit16-18；GPS L2C 不得误判为北斗。"""
    print('\n[case 2] RANGE 星座解析（含 GPS L2C(M) sys=0/sig=17 回归）')
    case = 'range_systems'

    # (prn, ch-tr-status, glofreq, psr, adr, dopp, cn0)
    obs = [
        (5,  ch_tr_status(0, 0),  0, 21000000.0, 1000.0,  2450.0, 45.0),  # GPS L1C/A
        (5,  ch_tr_status(0, 17), 0, 21000010.0, 2000.0,  1910.0, 43.5),  # GPS L2C(M) ← 回归关键
        (12, ch_tr_status(1, 0),  8, 22000000.0, 3000.0,  1600.0, 44.0),  # GLONASS L1C/A
        (3,  ch_tr_status(3, 12), 0, 23000000.0, 4000.0,  1750.0, 42.0),  # Galileo E5a
        (7,  ch_tr_status(4, 0),  0, 24000000.0, 5000.0,  2100.0, 46.0),  # BDS B1I
    ]
    bin_path = write_bin(out_dir, case, make_frame(MSG_ID_RANGE, range_body(obs)))

    rc, out, err = run_parser(exe, bin_path, out_dir)
    fx.eq(rc, 0, '退出码 = 0')

    path = csv_path(out_dir, case, 'range')
    fx.check(os.path.isfile(path), 'range.csv 已创建')
    if not os.path.isfile(path):
        return
    header, rows = read_csv(path)
    fx.eq(','.join(header),
          'GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,'
          'CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s,'
          'ChTrStatus,Sat_System_Name',
          'RANGE 表头（追加 ChTrStatus,Sat_System_Name 于行尾）')
    fx.eq(len(rows), 5, 'range 数据行数 = 5')

    systems = [r['Sat_System'] for r in rows]
    fx.eq(systems, ['0', '0', '2', '3', '1'],
          'Sat_System 依次 = 0,0,2,3,1（内部枚举 GPS,GPS,GLONASS,GALILEO,BDS）')

    # 首次出现的系统顺序（去掉同一系统的重复频点）应为 GPS,GLONASS,GALILEO,BDS
    order = []
    for s in systems:
        if s not in order:
            order.append(s)
    fx.eq(order, ['0', '2', '3', '1'],
          '系统首次出现顺序 = 0,2,3,1（GPS,GLONASS,GALILEO,BDS）')

    fx.eq([r['ChTrStatus'] for r in rows],
          ['0x00000000', '0x02200000', '0x00010000', '0x01830000', '0x00040000'],
          'ChTrStatus 十六进制 = 官方位域原样回显')

    # 关键回归断言：ch-tr-status = 0x02200000 的那条（sig=17 L2C）必须是 GPS(0)
    l2c_rows = [r for r in rows if r['ChTrStatus'] == '0x02200000']
    fx.eq(len(l2c_rows), 1, '找到那条 GPS L2C(M) 观测（ChTrStatus=0x02200000）')
    if l2c_rows:
        fx.eq(l2c_rows[0]['Sat_System'], '0',
              'GPS L2C(M) (sys=0,sig=17) 判为内部枚举 0 = GPS（不是 BDS=1）')
        fx.eq(l2c_rows[0]['Sat_System_Name'], SYSTEM_NAME[INT_GPS],
              'GPS L2C(M) 的 Sat_System_Name = GPS')
        fx.check(l2c_rows[0]['Sat_System'] != str(INT_BDS),
                 'GPS L2C(M) 未被误判成北斗')

    fx.eq([r['Sat_System_Name'] for r in rows],
          ['GPS', 'GPS', 'GLONASS', 'Galileo', '北斗'],
          'Sat_System_Name 与内部枚举一致且非空')
    fx.eq(rows[2]['GloFreq'], '8', 'GLONASS GloFreq = 8 (= GLONASS Frequency 1 + 7)')
    fx.eq(rows[0]['GloFreq'], '0', '非 GLONASS 的 GloFreq = 0')
    fx.eq(stat_value(out, 'malformed'), 0, 'malformed 帧数 = 0')
    fx.eq(stat_value(out, 'unsupported'), 0, 'unsupported 帧数 = 0')
    fx.eq(stat_value(out, 'crc_error'), 0, 'crc_error = 0')


def test_03_satvis2_table124(exe, out_dir, fx):
    """SATVIS2 系统字段按官方 Table 124 → 内部枚举，并校验新增列。"""
    print('\n[case 3] SATVIS2 系统字段（官方 Table 124: 0/1/2/5/6/7/9）')
    case = 'satvis2_systems'

    # (官方 Table 124 编号, 期望内部枚举, 期望名称)
    groups = [
        (0, INT_GPS,   'GPS',     2450.0),
        (1, INT_GLO,   'GLONASS', 1600.0),
        (2, INT_SBAS,  'SBAS',    1200.0),
        (5, INT_GAL,   'Galileo', 1750.0),
        (6, INT_BDS,   '北斗',     2100.0),
        (7, INT_QZSS,  'QZSS',    2500.0),
        (9, INT_NAVIC, 'NavIC',   1550.0),
    ]

    blob = b''
    for idx, (log_sys, _int_sys, _name, dopp) in enumerate(groups):
        sats = [
            (1 + idx * 4, 0, 0, 45.0, 100.0, dopp, dopp + 1.5),
            (2 + idx * 4, 0, 0, 20.0, 200.0, dopp + 10.0, dopp + 11.5),
        ]
        blob += make_frame(MSG_ID_SATVIS2, satvis2_body(log_sys, sats), tow_ms=idx * 1000)

    # 追加一颗**负仰角**卫星，覆盖真实情况（官方示例含 -82.3°）
    blob += make_frame(MSG_ID_SATVIS2,
                       satvis2_body(9, [(77, 0, 0, -15.0, 33.0, 1400.0, 1401.5)]),
                       tow_ms=7000)

    bin_path = write_bin(out_dir, case, blob)
    rc, out, err = run_parser(exe, bin_path, out_dir)
    fx.eq(rc, 0, '退出码 = 0')

    path = csv_path(out_dir, case, 'satvis2')
    fx.check(os.path.isfile(path), 'satvis2.csv 已创建')
    if not os.path.isfile(path):
        return
    header, rows = read_csv(path)
    fx.eq(','.join(header),
          'GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,Health,'
          'GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name',
          'SATVIS2 表头（追加 GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name）')
    fx.eq(len(rows), 15, 'satvis2 数据行数 = 15 (7组×2 + 1)')

    # 每个分组的 2 行（最后一组 1 行）系统应一致
    expected_systems = []
    for (log_sys, int_sys, _name, _dopp) in groups:
        expected_systems += [str(int_sys)] * 2
    expected_systems += [str(INT_NAVIC)]
    fx.eq([r['Sat_System'] for r in rows], expected_systems,
          'Sat_System 序列 = 0,2,4,3,1,5,6（每个系统两行，负仰角那颗为 NavIC）')

    expected_names = []
    for (log_sys, int_sys, name, _dopp) in groups:
        expected_names += [name] * 2
    expected_names += ['NavIC']
    fx.eq([r['Sat_System_Name'] for r in rows], expected_names,
          'Sat_System_Name 非空且与官方 Table 124 映射一致')

    # 每个系统的名称必须与其内部枚举映射表一致
    bad = [(r['Sat_System'], r['Sat_System_Name']) for r in rows
           if r['Sat_System_Name'] != SYSTEM_NAME.get(int(r['Sat_System']))]
    fx.check(not bad, 'Sat_System_Name 与内部枚举一一对应', '不一致项: %r' % (bad,))

    fx.check(all(r['Sat_System_Name'] for r in rows), 'Sat_System_Name 无空值')

    # Health 必须是整数（不能被 std::fixed 打成 88.000000）
    fx.check(all('.' not in r['Health'] and r['Health'] != '' for r in rows),
             'Health 以整数输出（无小数点）',
             'Health 列: %r' % ([r['Health'] for r in rows],))

    # 新增多普勒列有真实数值
    fx.close(rows[0]['TrueDoppler_Hz'], 2450.0, 1e-3, 'TrueDoppler_Hz = 2450.000 (GPS组)')
    fx.close(rows[0]['ApparentDoppler_Hz'], 2451.5, 1e-3, 'ApparentDoppler_Hz = 2451.500')
    fx.eq(rows[0]['GloFreq'], '0', '非 GLONASS 的 GloFreq = 0')

    # 负仰角必须原样保留
    fx.close(rows[-1]['Elevation_deg'], -15.0, 1e-6, '负仰角 -15.0 被正确保留')
    fx.eq(stat_value(out, 'unsupported'), 0, 'unsupported 帧数 = 0')
    fx.eq(list_csvs(out_dir).count('gnss_%s_range.csv' % case), 0,
          '未解析到 RANGE → 不创建 range.csv')


def test_04_satvis48_unsupported(exe, out_dir, fx):
    """SATVIS(48) 为 OEM6 旧日志：不得解析，计入 unsupported_frames。"""
    print('\n[case 4] SATVIS(48) 必须不被解析（计入 unsupported_frames）')
    case = 'satvis48'
    body = satvis48_body(4, [(1, 45.0, 100.0), (2, 30.0, 200.0)])
    bin_path = write_bin(out_dir, case, make_frame(MSG_ID_SATVIS, body))

    rc, out, err = run_parser(exe, bin_path, out_dir)
    fx.check(rc != 0, '退出码 != 0（无任何受支持的帧）', 'rc=%d' % rc)
    unsupported = stat_value(out, 'unsupported')
    fx.check(unsupported is not None and unsupported >= 1,
             'unsupported 帧数 >= 1', 'stat=%r\n           stdout:\n%s' % (unsupported, out))
    fx.eq(stat_value(out, 'malformed'), 0, 'malformed 帧数 = 0')
    fx.eq([n for n in list_csvs(out_dir) if case in n], [],
          'SATVIS(48) 未产生任何 CSV（含 satvis.csv）')
    fx.check('OEM6' in err or 'SATVIS' in err,
             '提示了 SATVIS(48) 为 OEM6 旧日志', 'stderr:\n%s' % err)


def test_05_bad_crc(exe, out_dir, fx):
    """CRC 被改坏的帧：计入 crc_error，且数据不得出现在 CSV 中。"""
    print('\n[case 5] CRC 损坏帧必须被拒（计入 crc_error 且数据不入 CSV）')
    case = 'bad_crc'

    good_obs = [(21, ch_tr_status(0, 0), 0, 25000000.0, 100.0, 2450.0, 45.0)]
    good = make_frame(MSG_ID_RANGE, range_body(good_obs))

    # 坏帧：合法内容 + 正确 CRC，然后故意把 CRC 最高字节改坏
    bad_obs = [(30, ch_tr_status(4, 0), 0, 26000000.0, 200.0, 2100.0, 44.0)]
    bad = make_frame(MSG_ID_RANGE, range_body(bad_obs))
    bad = bad[:-1] + bytes([bad[-1] ^ 0xFF])

    bin_path = write_bin(out_dir, case, good + bad)
    rc, out, err = run_parser(exe, bin_path, out_dir)

    crc_err = stat_value(out, 'crc_error')
    fx.check(crc_err is not None and crc_err >= 1,
             'crc_error >= 1', 'stat=%r\n           stdout:\n%s' % (crc_err, out))
    fx.eq(stat_value(out, 'malformed'), 0, 'malformed 帧数 = 0（仅 CRC 错误）')

    path = csv_path(out_dir, case, 'range')
    fx.check(os.path.isfile(path), 'range.csv 已创建（好帧仍在）')
    if not os.path.isfile(path):
        return
    header, rows = read_csv(path)
    fx.eq([r['Sat_PRN'] for r in rows], ['21'],
          '只有 CRC 正确的帧（PRN=21）进入 CSV')
    fx.check(all(r['Sat_PRN'] != '30' for r in rows),
             'CRC 损坏帧的数据（PRN=30）未出现在 CSV 中')


# ============================================================
# 主流程
# ============================================================

TESTS = [
    ('case 1', test_01_official_bestposb),
    ('case 2', test_02_range_systems),
    ('case 3', test_03_satvis2_table124),
    ('case 4', test_04_satvis48_unsupported),
    ('case 5', test_05_bad_crc),
]


def main():
    ap = argparse.ArgumentParser(description='NovAtel OEM7 BIN 解析工具回归测试')
    ap.add_argument('--exe', default=None, help='gnss_parser 可执行文件路径')
    ap.add_argument('--keep', action='store_true', help='保留临时输出目录')
    ap.add_argument('--outdir', default=None, help='指定输出目录（默认自动创建临时目录）')
    args = ap.parse_args()

    exe = args.exe or find_default_exe(__file__)
    if not exe or not os.path.isfile(exe):
        print('错误: 找不到被测可执行文件 gnss_parser。')
        print('  尝试过: %r' % (args.exe or find_default_exe(__file__),))
        print('  请先编译: cmake --build build --config Release  (或用 --exe 指定)')
        return 2

    print('=' * 72)
    print('NovAtel OEM7 BIN 解析工具 —— 回归测试')
    print('  被测程序: %s' % exe)
    print('=' * 72)

    temp_root = args.outdir or tempfile.mkdtemp(prefix='gnss_regression_')
    if args.outdir:
        os.makedirs(temp_root, exist_ok=True)
    print('  输出目录: %s' % temp_root)

    failures = Failures()
    test_00_official_crc_selftest(failures)

    for name, fn in TESTS:
        case_dir = os.path.join(temp_root, name.replace(' ', '_'))
        os.makedirs(case_dir, exist_ok=True)
        try:
            fn(exe, case_dir, failures)
        except Exception as exc:  # 任何异常都视为失败，便于定位
            import traceback
            print('    [FAIL] %s 抛出异常: %r' % (name, exc))
            traceback.print_exc()
            failures.items.append('%s 异常: %r' % (name, exc))

    print('\n' + '=' * 72)
    if failures.items:
        print('结果: FAIL（%d 项断言失败）' % len(failures.items))
        for item in failures.items:
            print('  - %s' % item)
        print('=' * 72)
        if args.keep or args.outdir:
            print('输出目录保留于: %s' % temp_root)
        else:
            shutil.rmtree(temp_root, ignore_errors=True)
        return 1

    print('结果: PASS（全部断言通过）')
    print('=' * 72)
    if args.keep or args.outdir:
        print('输出目录保留于: %s' % temp_root)
    else:
        shutil.rmtree(temp_root, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
