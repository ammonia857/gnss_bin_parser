"""Task 5：CSV 流式聚合与行索引分页测试。

相对计划原文有两处**期望值修正**（计划里的期望与其自身 fixture 不自洽，已在实现说明中记录）：
1) fixture 中 `sysv` 为 1 与 2 时写的都是「北斗」，所以 `systemNames["北斗"] == 8000`（不是 4000）；
2) `offset` 是 0 基数据行号，`offset=4999` 对应 `TOW_ms = 1000 + 4999 = 5999`。
"""
import tempfile
import unittest
from pathlib import Path

from gui.aggregate import build_row_index, page_rows, summarize

RANGE_HEADER = ("GPS_Week,TOW_ms,Sat_System,Sat_PRN,GloFreq,Pseudorange_m,PsrStd_m,"
                "CarrierPhase_cycle,AdrStd_cycle,Doppler_Hz,CN0_dBHz,LockTime_s,"
                "ChTrStatus,Sat_System_Name\n")


def write_range(path: Path, rows: int = 12000) -> None:
    """写一个 RANGE 风格 CSV：三系统轮转、末尾追加一行坏行。"""
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        handle.write(RANGE_HEADER)
        for i in range(rows):
            sysv = i % 3
            handle.write(f"2215,{1000 + i},{sysv},{i % 32 + 1},0,20000000.0,0.5,-1000.0,0.01,"
                         f"100.0,45.0,10.0,0x00000000,{'GPS' if sysv == 0 else '北斗'}\n")
        handle.write("BROKEN,LINE\n")


class AggregateTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.dir = Path(self.td.name)
        self.csv = self.dir / "t_range.csv"
        write_range(self.csv, 12000)

    def tearDown(self):
        self.td.cleanup()

    def test_summarize_counts_and_distribution(self):
        s = summarize(self.csv)
        self.assertEqual(s["rows"], 12000)
        self.assertEqual(s["skippedRows"], 1)
        self.assertEqual(s["timeSpan"]["towMin"], 1000)
        self.assertEqual(s["timeSpan"]["towMax"], 1000 + 11999)
        self.assertEqual(sum(s["systems"].values()), 12000)
        self.assertEqual(s["systems"]["0"], 4000)
        self.assertEqual(s["systems"]["1"], 4000)
        self.assertEqual(s["systemNames"].get("北斗"), 8000)
        self.assertEqual(s["columns"][0], "GPS_Week")
        self.assertEqual(s["columns"][-1], "Sat_System_Name")

    def test_summarize_without_system_columns(self):
        """bestpos 风格（无 Sat_System / Sat_System_Name）不应报错。"""
        path = self.dir / "t_bestpos.csv"
        path.write_text(
            "GPS_Week,TOW_ms,Solution_Status,SVs_Tracked\n"
            "2215,1000,0,20\n"
            "2215,2000,0,21\n", encoding="utf-8-sig")
        s = summarize(path)
        self.assertEqual(s["rows"], 2)
        self.assertEqual(s["systems"], {})
        self.assertEqual(s["systemNames"], {})
        self.assertEqual(s["timeSpan"]["towMax"], 2000)

    def test_summary_cache_invalidates_on_change(self):
        from gui.aggregate import SummaryCache
        cache = SummaryCache()
        a = summarize(self.csv, cache)
        b = summarize(self.csv, cache)
        self.assertIs(a, b)                     # 命中缓存，同一对象
        self.csv.write_text(RANGE_HEADER, encoding="utf-8")   # 改写文件（只剩表头）
        c = summarize(self.csv, cache)
        self.assertIsNot(a, c)
        self.assertEqual(c["rows"], 0)

    def test_row_index_and_paging(self):
        idx = build_row_index(self.csv, every=5000)
        self.assertEqual(len(idx), 3)           # 12000 行 / 5000 = 3 块
        page = page_rows(self.csv, idx, offset=4999, limit=3)
        self.assertEqual([r["TOW_ms"] for r in page], ["5999", "6000", "6001"])
        filtered = page_rows(self.csv, idx, offset=0, limit=5, filter_system=1)
        self.assertTrue(filtered)
        self.assertTrue(all(r["Sat_System"] == "1" for r in filtered))

    def test_paging_crosses_block_boundary_and_handles_eof(self):
        idx = build_row_index(self.csv, every=5000)
        page = page_rows(self.csv, idx, offset=4998, limit=4)
        self.assertEqual([r["TOW_ms"] for r in page], ["5998", "5999", "6000", "6001"])
        self.assertEqual(page_rows(self.csv, idx, offset=999999, limit=5), [])


if __name__ == "__main__":
    unittest.main()
