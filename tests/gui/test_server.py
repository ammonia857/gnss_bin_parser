"""Task 7：HTTP 服务接口测试（真实起服务 + 假引擎，走 HTTP 客户端）。"""
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

from gui.server import create_server

HERE = Path(__file__).resolve().parent
FAKE = str(HERE / "fake_engine.py")


def http(method, url, body=None, headers=None):
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8") or "{}")


def get(url):
    return http("GET", url)


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        cls.root = Path(cls.td.name)
        (cls.root / "gui" / "static").mkdir(parents=True)
        (cls.root / "gui" / "static" / "index.html").write_text("<html>ok</html>", encoding="utf-8")
        cls.state_path = cls.root / "gui" / "state.json"
        cls.srv, cls.port = create_server(cls.root, port=0, engine_cmd=[sys.executable, FAKE],
                                          state_path=cls.state_path)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.ctx.queue.stop()
        cls.srv.shutdown()
        cls.srv.server_close()
        cls.td.cleanup()

    def test_static_index(self):
        with urllib.request.urlopen(self.base + "/", timeout=10) as resp:
            self.assertEqual(resp.status, 200)
            self.assertIn("ok", resp.read().decode("utf-8"))

    def test_settings_roundtrip(self):
        status, obj = get(self.base + "/api/settings")
        self.assertEqual(status, 200)
        self.assertIn("autoStart", obj)
        status, obj = http("POST", self.base + "/api/settings",
                           {"autoStart": False, "outputDir": self.td.name})
        self.assertEqual(status, 200)
        self.assertFalse(obj["autoStart"])
        _, again = get(self.base + "/api/settings")
        self.assertEqual(again["outputDir"], self.td.name)

    def test_job_lifecycle_and_summary(self):
        binfile = Path(self.td.name) / "s.bin"
        binfile.write_bytes(b"\x00" * 2048)
        out_dir = Path(self.td.name) / "out"
        status, obj = http("POST", self.base + "/api/jobs",
                           {"paths": [str(binfile)], "outputDir": str(out_dir)})
        self.assertEqual(status, 200)
        jid = obj["jobs"][0]["id"]

        deadline = time.time() + 30
        job = None
        while time.time() < deadline:
            _, snap = get(self.base + "/api/jobs")
            job = next(j for j in snap["jobs"] if j["id"] == jid)
            if job["status"] in ("done", "failed"):
                break
            time.sleep(0.1)
        self.assertEqual(job["status"], "done", job.get("message"))

        status, summ = get(f"{self.base}/api/results/{jid}/summary")
        self.assertEqual(status, 200)
        self.assertEqual(summ["datasets"]["range"]["rows"], 4)
        self.assertEqual(summ["datasets"]["satvis2"]["rows"], 3)
        self.assertEqual(summ["datasets"]["bestpos"]["rows"], 1)
        self.assertEqual(summ["totalRows"], 8)
        self.assertEqual(len(summ["files"]), 3)

        status, page = get(f"{self.base}/api/results/{jid}/rows?dataset=range&offset=0&limit=2")
        self.assertEqual(status, 200)
        self.assertEqual(len(page["rows"]), 2)
        self.assertEqual(page["rows"][0]["GPS_Week"], "2215")

    def test_bad_request_returns_400(self):
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            http("POST", self.base + "/api/jobs", {"paths": []})
        self.assertEqual(ctx.exception.code, 400)

    def test_unknown_path_returns_404(self):
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            get(self.base + "/api/nope")
        self.assertEqual(ctx.exception.code, 404)

    def _job(self, jid):
        _, snap = get(self.base + "/api/jobs")
        return next(j for j in snap["jobs"] if j["id"] == jid)

    def _wait_status(self, jid, wanted, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            job = self._job(jid)
            if job["status"] in wanted:
                return job
            time.sleep(0.1)
        self.fail(f"任务 {jid} 未在 {timeout}s 内进入 {wanted}（当前 {self._job(jid)['status']}）")

    def test_failed_job_can_be_retried_then_removed(self):
        binfile = Path(self.td.name) / "r.bin"
        binfile.write_bytes(b"\x00" * 1024)
        out_dir = Path(self.td.name) / "out_r"
        os.environ["FAKE_ENGINE_FAIL"] = "1"          # 先让它失败
        try:
            _, obj = http("POST", self.base + "/api/jobs",
                          {"paths": [str(binfile)], "outputDir": str(out_dir)})
            jid = obj["jobs"][0]["id"]
            self._wait_status(jid, ("failed",))
        finally:
            os.environ.pop("FAKE_ENGINE_FAIL", None)
        # done 的任务不允许 retry；failed 的可以，重试后应变为 done
        status, _ = http("POST", f"{self.base}/api/jobs/{jid}/retry")
        self.assertEqual(status, 200)
        self._wait_status(jid, ("done",))
        status, _ = http("POST", f"{self.base}/api/jobs/{jid}/remove")
        self.assertEqual(status, 200)
        _, snap = get(self.base + "/api/jobs")
        self.assertFalse(any(j["id"] == jid for j in snap["jobs"]))

    def test_retry_on_done_job_is_rejected(self):
        binfile = Path(self.td.name) / "d.bin"
        binfile.write_bytes(b"\x00" * 1024)
        _, obj = http("POST", self.base + "/api/jobs",
                      {"paths": [str(binfile)], "outputDir": str(Path(self.td.name) / "out_d")})
        jid = obj["jobs"][0]["id"]
        self._wait_status(jid, ("done",))
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            http("POST", f"{self.base}/api/jobs/{jid}/retry")
        self.assertEqual(ctx.exception.code, 409)

    def test_second_instance_moves_to_next_port(self):
        """回归：端口已被占用时必须顺延到下一个端口。

        Windows 的 SO_REUSEADDR 允许抢占已监听端口，若不关掉，第二个实例会绑到同一
        端口并与之争抢连接（队列忽然变空、任务串台），顺延逻辑形同虚设。
        """
        srv2, port2 = create_server(self.root, port=self.port, engine_cmd=[sys.executable, FAKE],
                                    state_path=self.state_path)
        # 注意：必须先 serve_forever 才能调 shutdown()——socketserver 的 shutdown() 会等
        # serve_forever 的循环事件，未启动时永久阻塞（本测试第一版就踩了这个坑）。
        threading.Thread(target=srv2.serve_forever, daemon=True).start()
        try:
            self.assertNotEqual(port2, self.port, "第二个实例不得复用已占用的端口")
            with urllib.request.urlopen(f"http://127.0.0.1:{port2}/api/jobs", timeout=10) as resp:
                self.assertEqual(resp.status, 200)
        finally:
            srv2.ctx.queue.stop()
            srv2.shutdown()
            srv2.server_close()

    def test_client_abort_does_not_print_traceback(self):
        """回归：浏览器关掉 keep-alive 连接时，控制台不得刷出 ConnectionResetError 堆栈。

        （用户双击 start_gui.bat 起的控制台窗口里出现大段红色 traceback，会以为程序坏了。）
        """
        import contextlib
        import io
        import socket
        import struct

        buffer = io.StringIO()
        with contextlib.redirect_stderr(buffer):
            sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            sock.sendall(b"GET /api/jobs HTTP/1.1\r\nHost: 127.0.0.1\r\n")   # 请求故意不完整
            # SO_LINGER 0 → close() 直接发 RST，模拟浏览器强行断开
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            sock.close()
            time.sleep(0.5)                                    # 留给服务端线程处理断开
        self.assertEqual(buffer.getvalue(), "", "客户端断开不应产生任何 stderr 输出")


if __name__ == "__main__":
    unittest.main()
