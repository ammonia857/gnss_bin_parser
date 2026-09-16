import os, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent

class FakeEngineSmokeTest(unittest.TestCase):
    def test_fake_engine_runs_and_creates_csvs(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "demo.bin"
            src.write_bytes(b"\x00" * 4096)
            out = Path(td) / "out"
            r = subprocess.run([sys.executable, str(HERE / "fake_engine.py"),
                                "-i", str(src), "-o", str(out), "-p", "demo"],
                               capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("进度: 100%", r.stdout)
            self.assertIn("输出前缀: demo_demo", r.stdout)
            self.assertTrue((out / "demo_demo_range.csv").exists())
            self.assertTrue((out / "demo_demo_satvis2.csv").exists())
