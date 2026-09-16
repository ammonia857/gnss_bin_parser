"""Task 10：真实引擎端到端测试用的 BIN 夹具。

构帧逻辑**直接复用** ``tests/regression.py``（同一份官方 CRC-32 与位域代码），
避免出现两套"官方实现"互相漂移；本模块只负责把若干帧拼成一个可解析的 BIN 文件，
并给出该文件的**期望统计**，供 ``test_e2e.py`` 断言。

编号来源（务必区分两套官方表）：
- RANGE 的 ``ch-tr-status`` 星座位 bit16-18 → 官方 Table 156：0=GPS 1=GLO 2=SBAS 3=GAL 4=BDS 5=QZSS 6=NavIC；
- SATVIS2 的日志系统字段 → 官方 Table 124：0=GPS 1=GLO 2=SBAS 5=GAL 6=BDS 7=QZSS 9=NavIC；
- 输出 CSV 的 ``Sat_System`` 用**内部枚举**：GPS=0 BDS=1 GLONASS=2 Galileo=3 SBAS=4 QZSS=5 NavIC=6。
"""
from pathlib import Path

from tests.regression import (                       # noqa: F401（部分名字供测试方引用）
    MSG_ID_BESTPOS, MSG_ID_RANGE, MSG_ID_SATVIS, MSG_ID_SATVIS2,
    OFFICIAL_BESTPOSB, OFFICIAL_BESTPOSB_CRC, ch_tr_status, make_frame,
    range_body, satvis2_body, satvis48_body)

# ---------- 夹具参数（改动这里必须同步改 test_e2e.py 的期望值来源：两者同源，改一处即可） ----------

EPOCHS = 3                                            # RANGE 历元数
RANGE_OBS = [                                         # (PRN, 官方星座号, 信号号, GloFreq)
    (5, 0, 0, 0),                                     # GPS
    (12, 1, 0, 8),                                    # GLONASS（GloFreq = 频率+7）
    (20, 4, 0, 0),                                    # BDS（Table 156 里的 4）
    (30, 3, 0, 0),                                    # Galileo
]
SATVIS2_GROUPS = [                                    # (官方 Table 124 系统号, 卫星数)
    (0, 2),                                           # GPS
    (6, 3),                                           # BDS
]
BESTPOS_FRAMES = 1                                    # 官方 BESTPOSB 示例帧
SATVIS48_FRAMES = 1                                   # OEM6 旧日志：应计入 unsupported
BROKEN_CRC_FRAMES = 1                                 # CRC 被改坏：应计入 crc 错误且不出行

# 内部枚举 → 中文名（与引擎 Sat_System_Name 列一致）
NAME_OF_INTERNAL = {0: "GPS", 1: "北斗", 2: "GLONASS", 3: "Galileo", 4: "SBAS", 5: "QZSS", 6: "NavIC"}
# Table 156（RANGE ch-tr-status）官方号 → 内部枚举
RANGE_OFFICIAL_TO_INTERNAL = {0: 0, 1: 2, 2: 4, 3: 3, 4: 1, 5: 5, 6: 6}


def write_dataset(path, epochs: int = EPOCHS) -> dict:
    """写出一个包含多类帧的 BIN 文件，返回期望统计 ``Expected`` 字典。"""
    path = Path(path)
    data = bytearray()
    seq = 0

    range_names = []                                  # 每颗观测到的卫星对应的中文系统名
    for epoch in range(epochs):
        obs = []
        for (prn, official_sys, sig, glofreq) in RANGE_OBS:
            obs.append((prn, ch_tr_status(official_sys, sig), glofreq,
                        20_000_000.0 + prn * 1000.0, 100_000.0 + prn, 1500.0 + prn, 45.0))
            range_names.append(NAME_OF_INTERNAL[RANGE_OFFICIAL_TO_INTERNAL[official_sys]])
        data += make_frame(MSG_ID_RANGE, range_body(obs),
                           tow_ms=epoch * 1000, seq=seq)
        seq += 1

    satvis2_names = []
    for (log_system, count) in SATVIS2_GROUPS:
        sats = [(prn, 8 if log_system == 1 else 0, 0, 30.0 + prn, 120.0 + prn, 1000.0, 990.0)
                for prn in range(1, count + 1)]
        data += make_frame(MSG_ID_SATVIS2, satvis2_body(log_system, sats), seq=seq)
        seq += 1

    for _ in range(BESTPOS_FRAMES):
        # 注意：OFFICIAL_BESTPOSB 只是"帧头+body"（28+72 字节），CRC 尾部要单独拼上，
        # 这是回归脚本里已验证过的官方字节（CRC = 42 dc 4c 48）。
        data += OFFICIAL_BESTPOSB + OFFICIAL_BESTPOSB_CRC
        seq += 1

    for _ in range(SATVIS48_FRAMES):
        data += make_frame(MSG_ID_SATVIS, satvis48_body(0, [(1, 45.0, 90.0)]), seq=seq)
        seq += 1

    for _ in range(BROKEN_CRC_FRAMES):
        good = make_frame(MSG_ID_RANGE, range_body([(7, ch_tr_status(0, 0), 0, 1.0, 2.0, 3.0, 40.0)]),
                          seq=seq)
        data += good[:-4] + bytes([good[-4] ^ 0xFF, good[-3], good[-2], good[-1]])
        seq += 1

    # 尾部补一段噪声，验证解析器能靠同步头重新对齐而不崩
    data += bytes([0x00, 0xAA, 0x44, 0x99, 0x12, 0x34])

    path.write_bytes(bytes(data))
    return {
        "path": str(path),
        "epochs": epochs,
        "rangeRows": len(range_names),
        "rangeNames": range_names,
        "satvis2Rows": sum(c for _, c in SATVIS2_GROUPS),
        "bestposRows": BESTPOS_FRAMES,
        "crcErrors": BROKEN_CRC_FRAMES,
        "unsupported": SATVIS48_FRAMES,
        "sizeBytes": len(data),
    }


# 期望的星座名 → 行数（RANGE）
def expected_range_names(epochs: int = EPOCHS) -> dict:
    names = {}
    for (_, official_sys, _, _) in RANGE_OBS:
        key = NAME_OF_INTERNAL[RANGE_OFFICIAL_TO_INTERNAL[official_sys]]
        names[key] = names.get(key, 0) + epochs
    return names


def expected_satvis2_names() -> dict:
    names = {}
    for (log_system, count) in SATVIS2_GROUPS:
        # Table 124 → 内部枚举（0=GPS 1=GLO 2=SBAS 5=GAL 6=BDS 7=QZSS 9=NavIC）
        internal = {0: 0, 1: 2, 2: 4, 5: 3, 6: 1, 7: 5, 9: 6}[log_system]
        key = NAME_OF_INTERNAL[internal]
        names[key] = names.get(key, 0) + count
    return names
