import unittest
from pathlib import Path
from gui.jobs import parse_progress_line, parse_stats_line, sanitize_prefix, unique_prefix, detect_engine

class ParseTest(unittest.TestCase):
    def test_progress_line(self):
        self.assertEqual(parse_progress_line("  进度: 43% (123.45 MB / 395.55 MB)"), 43.0)
        self.assertEqual(parse_progress_line("进度: 100% (0.00 MB / 0.00 MB)"), 100.0)
        self.assertIsNone(parse_progress_line("开始解析..."))
        self.assertIsNone(parse_progress_line("  进度: 43%"))

    def test_stats_lines_with_ascii_tokens(self):
        st = {}
        for line in [
            "成功解析帧数: 431979",
            "  - RANGE观测帧:     86396",
            "  - SATVIS可见性帧:  0",
            "  - SATVIS2可见性帧: 259187",
            "  - BESTPOS定位帧:   86396",
            "同步丢失 (sync_lost):     0",
            "CRC校验失败 (crc_error):  0",
            "结构异常帧 (malformed):   0",
            "不支持的日志帧 (unsupported): 0",
            "CSV导出行数:",
            "  - RANGE行:     4313496",
            "  - SATVIS2行:   5010937",
            "  - BESTPOS行:  86396",
            "CSV 写入错误: 3",
        ]:
            parse_stats_line(line, st)
        self.assertEqual(st["totalFrames"], 431979)
        self.assertEqual(st["rangeFrames"], 86396)
        self.assertEqual(st["satvis2Frames"], 259187)
        self.assertEqual(st["bestposFrames"], 86396)
        self.assertEqual(st["syncLost"], 0)
        self.assertEqual(st["crcErrors"], 0)
        self.assertEqual(st["malformed"], 0)
        self.assertEqual(st["unsupported"], 0)
        self.assertEqual(st["rangeRows"], 4313496)
        self.assertEqual(st["satvis2Rows"], 5010937)
        self.assertEqual(st["bestposRows"], 86396)
        self.assertEqual(st["csvWriteErrors"], 3)

    def test_sanitize_and_unique_prefix(self):
        self.assertEqual(sanitize_prefix("Data_1Hz"), "Data_1Hz")
        self.assertEqual(sanitize_prefix("a/b:c*?.bin"), "a_b_c_")
        td = Path(__file__).resolve().parent
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            self.assertEqual(unique_prefix(out, "x"), "x")
            (out / "x_range.csv").write_text("h\n", encoding="utf-8")
            self.assertEqual(unique_prefix(out, "x"), "x-2")
            (out / "x-2_range.csv").write_text("h\n", encoding="utf-8")
            self.assertEqual(unique_prefix(out, "x"), "x-3")

    def test_detect_engine_prefers_env_and_build_paths(self):
        import os, tempfile
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "build" / "Release").mkdir(parents=True)
            exe = root / "build" / "Release" / "gnss_parser.exe"
            exe.write_bytes(b"MZ")
            self.assertEqual(detect_engine(root, ""), str(exe))
            os.environ["GNSS_PARSER_EXE"] = str(exe)
            try:
                self.assertEqual(detect_engine(root, ""), str(exe))
            finally:
                os.environ.pop("GNSS_PARSER_EXE", None)
            self.assertIsNone(detect_engine(root / "nope", ""))
