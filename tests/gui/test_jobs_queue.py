"""Task 4：串行任务队列的行为测试（用假引擎保证确定性）。

覆盖：串行执行与统计、失败任务报告、运行中取消、输出前缀防覆盖、重启中断恢复。
"""
import os, sys, tempfile, time, unittest
from pathlib import Path

from gui.jobs import JobQueue
from gui.state import State

HERE = Path(__file__).resolve().parent
FAKE = str(HERE / "fake_engine.py")


class QueueTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name)
        (self.root / "build" / "Release").mkdir(parents=True)
        # 用假引擎冒充真引擎（以 python 解释器启动），避免依赖编译产物
        self.exe = self.root / "build" / "Release" / "gnss_parser.exe"
        self.exe.write_text("", encoding="utf-8")
        self.state = State(self.root / "state.json")
        self.state.load()
        self.events = []
        self.q = JobQueue(self.state, self.root, self.events.append,
                          engine_cmd=[sys.executable, FAKE])
        self.q.start()

    def tearDown(self):
        self.q.stop()
        self.td.cleanup()

    def _bin(self, name="a.bin", size=2048):
        p = self.root / name
        p.write_bytes(b"\x00" * size)
        return str(p)

    def _wait(self, timeout=30):
        """等所有任务离开 queued/running；超时即失败。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if all(j["status"] not in ("queued", "running") for j in self.q.snapshot()):
                return
            time.sleep(0.05)
        self.fail("任务未在超时内结束")

    def test_serial_execution_and_stats(self):
        jobs = self.q.enqueue([self._bin("a.bin"), self._bin("b.bin")], str(self.root / "out"))
        self.assertEqual([j["status"] for j in jobs], ["queued", "queued"])
        self._wait()
        snap = self.q.snapshot()
        self.assertEqual([j["status"] for j in snap], ["done", "done"])
        self.assertEqual(snap[0]["stats"]["totalFrames"], 5)
        self.assertEqual(snap[0]["stats"]["rangeRows"], 4)
        self.assertEqual(snap[0]["progress"], 100.0)
        self.assertTrue((self.root / "out" / "gnss_a_range.csv").exists())
        self.assertTrue(any(e.get("type") == "progress" for e in self.events))

    def test_failed_job_reports_exit_code(self):
        os.environ["FAKE_ENGINE_FAIL"] = "1"
        try:
            self.q.enqueue([self._bin("bad.bin")], str(self.root / "out2"))
            self._wait()
            j = self.q.snapshot()[0]
            self.assertEqual(j["status"], "failed")
            self.assertEqual(j["stats"]["exitCode"], 1)
            self.assertIn("有效帧", j["message"])
        finally:
            os.environ.pop("FAKE_ENGINE_FAIL", None)

    def test_cancel_running_job(self):
        os.environ["FAKE_ENGINE_DELAY"] = "0.4"
        try:
            self.q.enqueue([self._bin("slow.bin")], str(self.root / "out3"))
            time.sleep(0.6)
            jid = self.q.snapshot()[0]["id"]
            self.assertTrue(self.q.cancel(jid))
            deadline = time.time() + 10
            while time.time() < deadline and self.q.snapshot()[0]["status"] == "running":
                time.sleep(0.05)
            self.assertEqual(self.q.snapshot()[0]["status"], "cancelled")
        finally:
            os.environ.pop("FAKE_ENGINE_DELAY", None)

    def test_prefix_collision_avoids_overwrite(self):
        (self.root / "out4").mkdir()
        (self.root / "out4" / "gnss_a_range.csv").write_text("old", encoding="utf-8")
        self.q.enqueue([self._bin("a.bin")], str(self.root / "out4"))
        self._wait()
        self.assertEqual(self.q.snapshot()[0]["prefix"], "gnss-2")
        self.assertEqual((self.root / "out4" / "gnss_a_range.csv").read_text(encoding="utf-8"), "old")

    def test_interrupted_recovery_marks_running_as_interrupted(self):
        self.state.set_jobs([{"id": "j_old", "status": "running", "inputPath": "x.bin"}])
        self.state.save()
        s2 = State(self.root / "state.json")
        s2.load()
        q2 = JobQueue(s2, self.root, lambda e: None, engine_cmd=[sys.executable, FAKE])
        self.assertEqual(q2.recover_interrupted(), 1)
        self.assertEqual(q2.snapshot()[0]["status"], "interrupted")

    def test_auto_start_off_holds_jobs_until_start_pending(self):
        """回归：关掉 autoStart 后紧接着入队，任务**不得**被执行（曾在设置生效前的阻塞等待里漏跑）。"""
        self.state.set_settings({"autoStart": False})
        self.q.enqueue([self._bin("hold.bin")], str(self.root / "out_hold"))
        time.sleep(0.6)                                  # 远大于 worker 轮询间隔
        self.assertEqual(self.q.snapshot()[0]["status"], "queued")

        self.assertEqual(self.q.start_pending(), 1)
        self._wait()
        self.assertEqual(self.q.snapshot()[0]["status"], "done")

    def test_prefix_collision_with_queued_jobs(self):
        """回归：两次连续入队的同名输入必须拿到不同前缀，否则第二个结果覆盖第一个。"""
        self.state.set_settings({"autoStart": False})     # 都停在排队态，文件系统里还没有 CSV
        first, second = self.q.enqueue([self._bin("same.bin")], str(self.root / "out_same")), \
            self.q.enqueue([self._bin("same.bin")], str(self.root / "out_same"))
        self.assertEqual(first[0]["prefix"], "gnss")
        self.assertEqual(second[0]["prefix"], "gnss-2")
        self.q.start_pending()
        self._wait()
        out = self.root / "out_same"
        self.assertTrue((out / "gnss_same_range.csv").exists())
        self.assertTrue((out / "gnss-2_same_range.csv").exists())


class PreflightTest(unittest.TestCase):
    """预检与错误路径（不需要启动工作线程）。"""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name)
        self.state = State(self.root / "state.json")
        self.state.load()
        self.q = JobQueue(self.state, self.root, lambda e: None,
                          engine_cmd=[sys.executable, FAKE])

    def tearDown(self):
        self.td.cleanup()

    def test_missing_file_is_failed_without_queueing(self):
        jobs = self.q.enqueue([str(self.root / "nope.bin")], str(self.root / "out"))
        self.assertEqual(jobs[0]["status"], "failed")
        self.assertIn("不存在", jobs[0]["message"])

    def test_empty_file_is_failed(self):
        empty = self.root / "empty.bin"
        empty.write_bytes(b"")
        jobs = self.q.enqueue([str(empty)], str(self.root / "out"))
        self.assertEqual(jobs[0]["status"], "failed")
        self.assertIn("为空", jobs[0]["message"])
