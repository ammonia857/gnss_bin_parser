"""假引擎：复刻 gnss_parser 的 stdout 与产出，供 GUI 测试确定性使用。

用法与真引擎一致： fake_engine.py -i <file> -o <dir> -p <prefix>
支持环境变量：FAKE_ENGINE_FAIL=1（exit 1 + 0 帧）、FAKE_ENGINE_DELAY=<秒>（每步延时）
"""
import argparse, os, sys, time

PROGRESS = [0, 25, 50, 75, 100]

def main() -> int:
    # 真引擎 stdout 恒为 UTF-8 字节；Python 管道下默认走 locale(cp936)，必须显式对齐
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", required=True)
    ap.add_argument("-o", default="./output")
    ap.add_argument("-p", default="gnss")
    a = ap.parse_args()

    delay = float(os.environ.get("FAKE_ENGINE_DELAY", "0"))
    fail = os.environ.get("FAKE_ENGINE_FAIL") == "1"

    # 真引擎命名规则：实际前缀 = -p 值 + '_' + 输入文件主名（无去重，无条件拼接）
    stem = os.path.splitext(os.path.basename(a.i))[0]
    prefix = f"{a.p}_{stem}"

    size_mb = os.path.getsize(a.i) / 1048576 if os.path.exists(a.i) else 0.0
    print(f"解析文件: {a.i}", flush=True)
    print(f"输出目录: {a.o}", flush=True)
    print(f"输出前缀: {prefix}", flush=True)
    for p in PROGRESS:
        time.sleep(delay)
        print(f"  进度: {p}% ({size_mb * p / 100:.2f} MB / {size_mb:.2f} MB)", flush=True)

    os.makedirs(a.o, exist_ok=True)
    print("========== 解析完成 ==========", flush=True)
    if fail:
        print("成功解析帧数: 0", flush=True)
        print("解析失败或未找到有效帧。", flush=True)
        return 1

    rows = {"range": 4, "satvis": 0, "satvis2": 3, "bestpos": 1}
    files = []
    for kind, n in rows.items():
        if n == 0:
            continue
        path = os.path.join(a.o, f"{prefix}_{kind}.csv")
        with open(path, "w", encoding="utf-8-sig", newline="") as f:
            if kind == "range":
                f.write("GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,"
                        "CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s,"
                        "ChTrStatus,Sat_System_Name\n")
                f.write("2215,1000,0,1,0,20000000.0,0.5,-1000.0,0.01,100.0,45.0,10.0,0x00000000,GPS\n")
                f.write("2215,1000,1,5,0,21000000.0,0.5,-2000.0,0.01,120.0,44.0,10.0,0x00040000,北斗\n")
                f.write("2215,2000,0,2,0,20100000.0,0.5,-1100.0,0.01,110.0,46.0,11.0,0x00000000,GPS\n")
                f.write("2215,2000,1,6,0,21100000.0,0.5,-2100.0,0.01,130.0,43.0,11.0,0x00040000,北斗\n")
            elif kind == "satvis2":
                f.write("GPS_Week,TOW_ms,Sat_System,Sat_PRN,Elevation_deg,Azimuth_deg,Health,"
                        "GloFreq,TrueDoppler_Hz,ApparentDoppler_Hz,Sat_System_Name\n")
                f.write("2215,1000,0,1,45.0,120.0,0,0,1000.0,1000.5,GPS\n")
                f.write("2215,1000,1,5,30.0,220.0,0,0,900.0,900.5,北斗\n")
                f.write("2215,2000,0,2,50.0,130.0,0,0,1010.0,1010.5,GPS\n")
            else:
                f.write("GPS_Week,TOW_ms,Solution_Status,Position_Type,Latitude_deg,Longitude_deg,"
                        "Height_m,Undulation_m,LatStd_m,LonStd_m,HgtStd_m,SVs_Tracked,SVs_Used\n")
                f.write("2215,1000,0,16,30.5,114.35,39.6,-14.4,0.9,0.9,1.9,20,18\n")
        files.append(path)

    print("成功解析帧数: 5", flush=True)     # = 2 RANGE + 0 SATVIS + 2 SATVIS2 + 1 BESTPOS（与分项自洽）
    print("  - RANGE观测帧:     2", flush=True)
    print("  - SATVIS可见性帧:  0", flush=True)
    print("  - SATVIS2可见性帧: 2", flush=True)
    print("  - BESTPOS定位帧:   1", flush=True)
    print("同步丢失 (sync_lost):     0", flush=True)
    print("CRC校验失败 (crc_error):  0", flush=True)
    print("结构异常帧 (malformed):   0", flush=True)
    print("不支持的日志帧 (unsupported): 0", flush=True)
    print("CSV导出行数:", flush=True)
    print(f"  - RANGE行:     {rows['range']}", flush=True)
    print(f"  - SATVIS行:    {rows['satvis']}", flush=True)
    print(f"  - SATVIS2行:   {rows['satvis2']}", flush=True)
    print(f"  - BESTPOS行:   {rows['bestpos']}", flush=True)
    print("输出文件（惰性创建，仅列出实际生成了数据的文件）:", flush=True)
    for p in files:
        print(f"  {p}", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
