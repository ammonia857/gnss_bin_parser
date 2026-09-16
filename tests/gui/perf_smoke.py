"""Task 10：真实大数据下的性能冒烟（手动运行，不进 unittest 默认套件）。

跑法：

    python tests/gui/perf_smoke.py "C:\\Users\\<你>\\Downloads\\Data_1Hz.bin"
    python tests/gui/perf_smoke.py <bin> --engine <gnss_parser.exe> --keep

测量项（都走界面同一条链路：JobQueue 起子进程 → 落盘 CSV → aggregate 统计/分页）：

1. 解析：总耗时、吞吐（MB/s）、帧数、RANGE 行数；
2. 概览统计 ``summarize``：首次（冷）与二次（命中缓存）耗时；
3. 行号索引 ``build_row_index`` 耗时；
4. 分页读取 ``page_rows``：首页 / 中部 / 尾部耗时（验证"不随页码变慢"）；
5. 结果 JSON 打到 stdout，便于贴进报告。

默认把输出写到临时目录并在结束时删除；``--keep`` 保留以便排查。
"""
import argparse
import json
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from gui import aggregate                                   # noqa: E402
from gui.jobs import detect_engine                          # noqa: E402
from gui.server import create_server                        # noqa: E402


def memory_peak_mb() -> float | None:
    """本进程内存峰值（MB）；仅 Windows 可测，其他平台返回 None。"""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    # 必须显式声明参数/返回类型：句柄在 64 位下是 8 字节，交给 ctypes 默认的 c_int 会被截断，
    # GetProcessMemoryInfo 于是失败（第一次实现就踩了这个坑）。
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE,
                                           ctypes.POINTER(PROCESS_MEMORY_COUNTERS),
                                           wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    counters = PROCESS_MEMORY_COUNTERS()
    counters.cb = ctypes.sizeof(counters)
    if not psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        return None
    return round(counters.PeakWorkingSetSize / 1048576, 1)


def main() -> int:
    parser = argparse.ArgumentParser(description="GNSS GUI 大数据性能冒烟")
    parser.add_argument("bin_file", help="待解析的 BIN 文件（几百 MB 级）")
    parser.add_argument("--engine", default="", help="gnss_parser.exe 路径（默认自动探测）")
    parser.add_argument("--out", default="", help="输出目录（默认临时目录）")
    parser.add_argument("--keep", action="store_true", help="保留输出目录")
    parser.add_argument("--port", type=int, default=0, help="服务端口（默认随机）")
    args = parser.parse_args()

    bin_file = Path(args.bin_file).resolve()
    if not bin_file.is_file():
        print(f"找不到文件: {bin_file}")
        return 2
    engine = args.engine or detect_engine(REPO)
    if not engine:
        print("找不到 gnss_parser.exe，请用 --engine 指定")
        return 2

    tmp = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="gnss_perf_"))
    tmp.mkdir(parents=True, exist_ok=True)
    result = {"bin": str(bin_file), "sizeMB": round(bin_file.stat().st_size / 1048576, 1),
              "engine": str(engine), "outDir": str(tmp)}

    srv, port = create_server(REPO, port=args.port, engine_cmd=[str(engine)],
                              state_path=tmp / "state.json")
    base = f"http://127.0.0.1:{port}"
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    queue = srv.ctx.queue

    t0 = time.monotonic()
    jobs = queue.enqueue([str(bin_file)], str(tmp))
    job_id = jobs[0]["id"]
    print(f"入队 {bin_file.name}（{result['sizeMB']} MB），任务 {job_id} …", flush=True)

    last_print = 0.0
    while True:
        job = next((j for j in queue.snapshot() if j["id"] == job_id), None)
        if job and job["status"] in ("done", "failed", "cancelled", "interrupted"):
            break
        now = time.monotonic()
        if job and now - last_print > 10:
            last_print = now
            print(f"  … {job['progress']:.1f}%  已用 {now - t0:.0f}s", flush=True)
        time.sleep(0.3)

    parse_sec = time.monotonic() - t0
    stats = job["stats"]
    result["parse"] = {
        "status": job["status"], "message": job["message"], "exitCode": stats["exitCode"],
        "seconds": round(parse_sec, 1),
        "engineSeconds": stats["elapsedSec"],
        "throughputMBps": round(result["sizeMB"] / parse_sec, 1) if parse_sec else None,
        "totalFrames": stats["totalFrames"], "rangeRows": stats["rangeRows"],
        "satvis2Rows": stats["satvis2Rows"], "bestposRows": stats["bestposRows"],
        "crcErrors": stats["crcErrors"], "elapsedSec": stats["elapsedSec"],
    }
    print(f"解析完成: {job['status']} {parse_sec:.1f}s "
          f"({result['parse']['throughputMBps']} MB/s)，帧 {stats['totalFrames']}，"
          f"RANGE 行 {stats['rangeRows']}", flush=True)

    files = srv.ctx.output_files(job)
    result["files"] = [{"name": p.name, "sizeMB": round(p.stat().st_size / 1048576, 1)}
                       for p in files]
    print("输出:", json.dumps(result["files"], ensure_ascii=False), flush=True)

    # ---- 聚合与分页性能 ----
    rng = next((p for p in files if p.name.endswith("_range.csv")), None)
    if rng is not None:
        t = time.monotonic()
        summary = aggregate.summarize(rng, srv.ctx.cache)
        cold = time.monotonic() - t
        t = time.monotonic()
        aggregate.summarize(rng, srv.ctx.cache)
        warm = time.monotonic() - t
        result["summary"] = {
            "rows": summary["rows"], "csvMB": round(rng.stat().st_size / 1048576, 1),
            "coldSeconds": round(cold, 2), "cachedSeconds": round(warm, 4),
            "systemNames": summary["systemNames"], "timeSpan": summary["timeSpan"],
            "mbPerSecond": round(rng.stat().st_size / 1048576 / cold, 1) if cold else None,
        }
        print(f"概览统计: {summary['rows']} 行 / {result['summary']['csvMB']} MB，"
              f"冷 {cold:.2f}s，命中缓存 {warm:.4f}s", flush=True)

        t = time.monotonic()
        index = aggregate.build_row_index(rng)
        index_sec = time.monotonic() - t
        pages = {}
        for label, offset in (("first", 0), ("middle", len(index) // 2), ("last", max(0, len(index) - 100))):
            t = time.monotonic()
            rows = aggregate.page_rows(rng, index, offset, 100)
            pages[label] = {"offset": offset, "rows": len(rows), "seconds": round(time.monotonic() - t, 4)}
        result["paging"] = {"indexedRows": len(index), "indexSeconds": round(index_sec, 2), "pages": pages}
        print(f"分页: 索引 {len(index)} 行用 {index_sec:.2f}s；首页 {pages['first']['seconds']}s / "
              f"中部 {pages['middle']['seconds']}s / 尾部 {pages['last']['seconds']}s", flush=True)

    queue.stop()
    srv.shutdown()
    srv.server_close()

    result["guiProcessPeakMB"] = memory_peak_mb()
    print(f"界面进程内存峰值: {result['guiProcessPeakMB']} MB（计划要求 < 500MB）", flush=True)

    if not args.keep and not args.out:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"输出保留在 {tmp}")
    print("\n=== PERF JSON ===")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if job["status"] == "done" else 1


if __name__ == "__main__":
    raise SystemExit(main())
