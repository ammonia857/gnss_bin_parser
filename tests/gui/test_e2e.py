"""Task 10：真实引擎端到端测试。

与 ``test_server.py``（假引擎、只测接口）不同，这里用**真实的 gnss_parser.exe** 解析
``fixtures.write_dataset`` 造的 BIN，走完整链路：HTTP 入队 → 队列起子进程 → 解析 stdout →
落盘 CSV → 统计与分页接口。校验点覆盖：

1. 统计数字与夹具期望逐项相等（帧数、各类型行数、CRC 错误、不支持帧）；
2. 输出文件名符合实测规则 ``{prefix}_{输入主名}_{类型}.csv``；
3. CSV 字节事实：UTF-8 BOM + CRLF；
4. 星座分布（``Sat_System_Name``）与夹具里安排的星座集合一致；
5. 分页读取（``offset/limit``）与 ``filterSystem`` 过滤；
6. 文档里写的列数（RANGE 14 / SATVIS2 11 / BESTPOS 13）与真实输出一致。

找不到 ``gnss_parser.exe`` 时整类跳过（未编译的机器上不应因此判失败）。
"""
import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request
from pathlib import Path

from gui.jobs import detect_engine
from gui.server import create_server
from tests.gui import fixtures

REPO = Path(__file__).resolve().parents[2]
ENGINE = os.environ.get("GNSS_E2E_ENGINE") or detect_engine(REPO)


def post(base, path, body=None):
    req = urllib.request.Request(base + path, data=json.dumps(body or {}).encode("utf-8"),
                                 method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read().decode("utf-8") or "{}")


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=120) as resp:
        return json.loads(resp.read().decode("utf-8") or "{}")


@unittest.skipUnless(ENGINE, "未找到 gnss_parser.exe（先按 README 编译，或用 GNSS_E2E_ENGINE 指定）")
class RealEngineE2ETest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory(prefix="gnss_gui_e2e_")
        root = Path(cls.td.name)
        cls.expected = fixtures.write_dataset(root / "demo.bin")
        cls.out_dir = root / "out"

        cls.srv, cls.port = create_server(REPO, port=0, state_path=root / "state.json")
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.port}"

        post(cls.base, "/api/settings",
             {"enginePath": str(ENGINE), "outputDir": str(cls.out_dir), "autoStart": True})
        created = post(cls.base, "/api/jobs",
                       {"paths": [cls.expected["path"]], "outputDir": str(cls.out_dir)})
        cls.job_id = created["jobs"][0]["id"]

        deadline = time.time() + 120
        while time.time() < deadline:
            job = cls.job()
            if job["status"] in ("done", "failed", "cancelled"):
                break
            time.sleep(0.1)
        cls.job_data = cls.job()

    @classmethod
    def tearDownClass(cls):
        cls.srv.ctx.queue.stop()
        cls.srv.shutdown()
        cls.srv.server_close()
        cls.td.cleanup()

    @classmethod
    def job(cls):
        return next((j for j in get(cls.base, "/api/jobs")["jobs"] if j["id"] == cls.job_id), None)

    # ---------- 1. 任务终态与统计 ----------

    def test_01_job_reaches_done_with_expected_stats(self):
        job = self.job_data
        self.assertEqual(job["status"], "done", f"任务未成功: {job['status']} {job['message']}")
        self.assertEqual(job["message"], "")
        s = job["stats"]
        self.assertEqual(s["exitCode"], 0)
        # 成功解析帧数 = RANGE 历元 + SATVIS2 帧 + BESTPOS 帧（unsupported / 坏 CRC 不计入）
        self.assertEqual(s["totalFrames"],
                         self.expected["epochs"] + len(fixtures.SATVIS2_GROUPS) + fixtures.BESTPOS_FRAMES)
        self.assertEqual(s["rangeRows"], self.expected["rangeRows"])
        self.assertEqual(s["satvis2Rows"], self.expected["satvis2Rows"])
        self.assertEqual(s["bestposRows"], self.expected["bestposRows"])
        self.assertEqual(s["crcErrors"], self.expected["crcErrors"])
        self.assertEqual(s["unsupported"], self.expected["unsupported"])
        self.assertGreaterEqual(s["syncLost"], 1)        # 夹具尾部故意留了噪声
        self.assertGreaterEqual(s["elapsedSec"], 0.0)
        self.assertEqual(job["prefix"], "gnss")          # unique_prefix 的默认候选

    def test_02_output_file_names_follow_measured_rule(self):
        # 实测规则：-p <p> 的产出是 {p}_{输入主名}_{类型}.csv，且惰性创建（BESTPOS 有 1 帧才会出现）
        names = sorted(p.name for p in self.out_dir.glob("*.csv"))
        self.assertEqual(names, ["gnss_demo_bestpos.csv", "gnss_demo_range.csv", "gnss_demo_satvis2.csv"])
        # 引擎 stdout 的"输出文件"段应被队列解析出来（供界面直接打开）
        listed = [Path(p).name for p in self.job_data["stats"]["outputFiles"]]
        self.assertEqual(sorted(listed), names)

    def test_03_csv_bytes_are_bom_and_crlf(self):
        for name in ("gnss_demo_range.csv", "gnss_demo_satvis2.csv", "gnss_demo_bestpos.csv"):
            raw = (self.out_dir / name).read_bytes()
            self.assertTrue(raw.startswith(b"\xef\xbb\xbf"), f"{name} 缺少 UTF-8 BOM")
            self.assertIn(b"\r\n", raw, f"{name} 未使用 CRLF")
            self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"), f"{name} 混用了 LF 与 CRLF")

    def test_04_csv_columns_match_readme(self):
        docs = {"gnss_demo_range.csv": 14, "gnss_demo_satvis2.csv": 11, "gnss_demo_bestpos.csv": 13}
        for name, cols in docs.items():
            first = (self.out_dir / name).read_text(encoding="utf-8-sig").splitlines()[0]
            self.assertEqual(len(first.split(",")), cols, f"{name} 列数与 README 不一致: {first}")

    # ---------- 2. 聚合与分页接口 ----------

    def test_05_summary_distribution_matches_fixture(self):
        summary = get(self.base, f"/api/results/{self.job_id}/summary")
        self.assertEqual(summary["totalRows"],
                         self.expected["rangeRows"] + self.expected["satvis2Rows"] + self.expected["bestposRows"])
        rng = summary["datasets"]["range"]
        self.assertEqual(rng["rows"], self.expected["rangeRows"])
        self.assertEqual(rng["systemNames"], fixtures.expected_range_names())
        self.assertEqual(summary["datasets"]["satvis2"]["systemNames"], fixtures.expected_satvis2_names())
        self.assertEqual(summary["datasets"]["bestpos"]["rows"], self.expected["bestposRows"])
        self.assertEqual({f["name"] for f in summary["files"]},
                         {"gnss_demo_range.csv", "gnss_demo_satvis2.csv", "gnss_demo_bestpos.csv"})
        for f in summary["files"]:
            self.assertEqual(f["sizeBytes"], (self.out_dir / f["name"]).stat().st_size)

    def test_06_rows_paging_and_filter(self):
        total = self.expected["rangeRows"]
        page = get(self.base, f"/api/results/{self.job_id}/rows?dataset=range&offset=0&limit=5")
        self.assertEqual(len(page["rows"]), 5)
        self.assertEqual(page["rows"][0]["Sat_System_Name"], "GPS")
        tail = get(self.base, f"/api/results/{self.job_id}/rows?dataset=range&offset={total - 2}&limit=5")
        self.assertEqual(len(tail["rows"]), 2)           # 越界自动截断
        beyond = get(self.base, f"/api/results/{self.job_id}/rows?dataset=range&offset={total + 10}&limit=5")
        self.assertEqual(beyond["rows"], [])

        # filterSystem 用内部枚举：1 = 北斗
        bds = get(self.base, f"/api/results/{self.job_id}/rows?dataset=range&offset=0&limit=100&filterSystem=1")
        expect_bds = fixtures.expected_range_names().get("北斗", 0)
        self.assertEqual(len(bds["rows"]), expect_bds)
        for row in bds["rows"]:
            self.assertEqual(row["Sat_System_Name"], "北斗")

    def test_07_summary_is_cached(self):
        first = get(self.base, f"/api/results/{self.job_id}/summary")
        second = get(self.base, f"/api/results/{self.job_id}/summary")
        self.assertEqual(first, second)
        # 缓存键含 (路径, size, mtime_ns)：清掉缓存重算必须得到同样的结果
        self.srv.ctx.cache.clear()
        third = get(self.base, f"/api/results/{self.job_id}/summary")
        self.assertEqual(third, first)

    # ---------- 3. 与"直接跑引擎"的一致性与失败路径 ----------

    def test_08_gui_output_identical_to_direct_engine_run(self):
        """界面只是包装：同一夹具经界面与直接跑 exe，CSV 必须逐字节相同。"""
        direct = Path(self.td.name) / "direct"
        subprocess.run([str(ENGINE), "-i", self.expected["path"], "-o", str(direct), "-p", "d"],
                       check=True, capture_output=True,
                       cwd=str(REPO))
        for kind in ("range", "satvis2", "bestpos"):
            mine = (self.out_dir / f"gnss_demo_{kind}.csv").read_bytes()
            theirs = (direct / f"d_demo_{kind}.csv").read_bytes()
            self.assertEqual(mine, theirs, f"{kind} CSV 与直接运行引擎的结果不一致")
        self.assertFalse((direct / "d_demo_satvis.csv").exists(), "SATVIS(48) 不应产出 CSV")

    def test_09_bad_inputs_fail_with_readable_message(self):
        root = Path(self.td.name)
        empty = root / "empty.bin"
        empty.write_bytes(b"")
        garbage = root / "garbage.bin"
        garbage.write_bytes(b"A" * 5000)
        created = post(self.base, "/api/jobs",
                       {"paths": [str(empty), str(garbage)], "outputDir": str(root / "bad_out")})
        results = []
        for job in created["jobs"]:
            deadline = time.time() + 60
            while time.time() < deadline:
                current = next((j for j in get(self.base, "/api/jobs")["jobs"] if j["id"] == job["id"]), None)
                if current and current["status"] in ("done", "failed", "cancelled"):
                    results.append(current)
                    break
                time.sleep(0.1)
            else:
                self.fail("坏输入任务超时未结束")
        self.assertEqual([r["status"] for r in results], ["failed", "failed"])
        self.assertIn("空", results[0]["message"])            # 0 字节文件：预检直接拦下
        self.assertTrue(results[1]["message"])               # 垃圾文件：引擎 0 帧 → 失败原因非空


@unittest.skipUnless(ENGINE, "未找到 gnss_parser.exe")
class SecondRunPrefixTest(unittest.TestCase):
    """同名输入再次入队时必须换前缀，否则会覆盖上一轮 CSV。"""

    def test_unique_prefix_on_second_enqueue(self):
        td = tempfile.TemporaryDirectory(prefix="gnss_gui_e2e2_")
        self.addCleanup(td.cleanup)
        root = Path(td.name)
        fixtures.write_dataset(root / "demo.bin")

        srv, port = create_server(REPO, port=0, state_path=root / "state.json")
        base = f"http://127.0.0.1:{port}"
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        self.addCleanup(srv.ctx.queue.stop)

        post(base, "/api/settings", {"enginePath": str(ENGINE), "autoStart": False})
        first = post(base, "/api/jobs", {"paths": [str(root / "demo.bin")], "outputDir": str(root / "out")})
        second = post(base, "/api/jobs", {"paths": [str(root / "demo.bin")], "outputDir": str(root / "out")})
        self.assertEqual(first["jobs"][0]["prefix"], "gnss")
        self.assertEqual(second["jobs"][0]["prefix"], "gnss-2")

        # autoStart 关闭时任务必须停在排队态，且 /api/jobs/start 能放行
        job = next(j for j in get(base, "/api/jobs")["jobs"] if j["id"] == first["jobs"][0]["id"])
        self.assertEqual(job["status"], "queued")
        post(base, "/api/jobs/start")

        deadline = time.time() + 60
        while time.time() < deadline:
            states = {j["id"]: j["status"] for j in get(base, "/api/jobs")["jobs"]}
            if states.get(second["jobs"][0]["id"]) in ("done", "failed"):
                break
            time.sleep(0.1)
        states = {j["id"]: j["status"] for j in get(base, "/api/jobs")["jobs"]}
        self.assertEqual(states[first["jobs"][0]["id"]], "done", states)
        self.assertEqual(states[second["jobs"][0]["id"]], "done", states)

        names = sorted(p.name for p in (root / "out").glob("*.csv"))
        self.assertIn("gnss_demo_range.csv", names)
        self.assertIn("gnss-2_demo_range.csv", names)     # 两轮结果都在，没有互相覆盖


if __name__ == "__main__":
    unittest.main()
