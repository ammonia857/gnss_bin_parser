"""Task 8：前端静态资源的契约测试。

不启动浏览器，只校验三件事：
1. 三个静态文件存在且被 ``index.html`` 正确引用；
2. ``app.js`` 语法可解析（有 node 时用 ``node --check``，否则退化为无操作）；
3. 前端用到的每个后端接口路径都在代码里出现（防止改后端漏改前端）。
"""
import json
import shutil
import subprocess
import tempfile
import threading
import unittest
import urllib.request
from pathlib import Path

from gui.server import create_server

REPO = Path(__file__).resolve().parents[2]
STATIC = REPO / "gui" / "static"


class StaticAssetTest(unittest.TestCase):
    def test_three_assets_exist(self):
        for name in ("index.html", "app.js", "style.css"):
            path = STATIC / name
            self.assertTrue(path.is_file(), f"缺少前端文件: {path}")
            self.assertGreater(path.stat().st_size, 200, f"{name} 内容过少")

    def test_index_references_css_and_js(self):
        html = (STATIC / "index.html").read_text(encoding="utf-8")
        self.assertIn("/static/style.css", html)
        self.assertIn("/static/app.js", html)
        for anchor in ("job-list", "stat-cards", "system-bars", "file-list", "drop-overlay", "banner"):
            self.assertIn(f'id="{anchor}"', html, f"index.html 缺少容器 #{anchor}")

    def test_app_js_syntax_is_valid(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("未安装 node，跳过语法检查")
        proc = subprocess.run([node, "--check", str(STATIC / "app.js")],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)

    def test_app_js_covers_all_endpoints(self):
        js = (STATIC / "app.js").read_text(encoding="utf-8")
        for endpoint in ("/api/settings", "/api/jobs", "/api/jobs/clear", "/api/jobs/start",
                         "/api/events", "/api/pick/files", "/api/pick/folder", "/api/pick/outdir",
                         "/api/upload", "/api/open", "/api/results/", "/api/jobs/"):
            self.assertIn(endpoint, js, f"app.js 未引用后端接口 {endpoint}")

    def test_no_external_network_dependency(self):
        """离线可用：不得引用 CDN 或外链资源。"""
        for name in ("index.html", "app.js", "style.css"):
            text = (STATIC / name).read_text(encoding="utf-8")
            self.assertNotIn("http://", text, f"{name} 出现 http 外链")
            self.assertNotIn("https://", text, f"{name} 出现 https 外链")

    def test_every_dom_id_used_by_js_exists_in_html(self):
        """app.js 里 ``$("id")`` 引用的每个元素都必须在 index.html 中定义（防拼写漂移）。"""
        import re

        js = (STATIC / "app.js").read_text(encoding="utf-8")
        html = (STATIC / "index.html").read_text(encoding="utf-8")
        ids = sorted(set(re.findall(r'\$\("([A-Za-z0-9_-]+)"\)', js)))
        self.assertGreater(len(ids), 8, "未解析到足够的 DOM id，检查 app.js 写法")
        missing = [i for i in ids if f'id="{i}"' not in html]
        self.assertEqual(missing, [], f"index.html 缺少这些 id: {missing}")


class StaticServingTest(unittest.TestCase):
    """真实仓库根目录起服务：确认三个静态文件能按正确 Content-Type 取回。"""

    @classmethod
    def setUpClass(cls):
        cls.srv, cls.port = create_server(REPO, port=0, engine_cmd=["python", "-c", ""])
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.ctx.queue.stop()
        cls.srv.shutdown()
        cls.srv.server_close()

    def fetch(self, path):
        with urllib.request.urlopen(self.base + path, timeout=10) as resp:
            return resp.status, resp.headers.get("Content-Type", ""), resp.read()

    def test_index_served_at_root(self):
        status, ctype, body = self.fetch("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", ctype)
        self.assertIn("charset=utf-8", ctype)
        self.assertIn("GNSS".encode("utf-8"), body)

    def test_css_and_js_served_with_charset(self):
        status, ctype, body = self.fetch("/static/style.css")
        self.assertEqual((status, "text/css" in ctype), (200, True))
        self.assertIn("charset=utf-8", ctype)
        status, ctype, body = self.fetch("/static/app.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", ctype)
        self.assertIn(b"EventSource", body)


class StubPicker:
    """假对话框：返回固定路径，记录收到的标题。"""

    def __init__(self, folder):
        self.folder = folder
        self.titles = []

    def pick_files(self):
        return []

    def pick_folder(self, timeout=600, title=None):
        self.titles.append(title)
        return self.folder


class PickerRouteTest(unittest.TestCase):
    """新增的 /api/pick/outdir 与 /api/jobs/start 路由（注入假 picker，绝不弹真窗口）。"""

    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        root = Path(cls.td.name)
        (root / "gui" / "static").mkdir(parents=True)
        (root / "gui" / "static" / "index.html").write_text("<html>ok</html>", encoding="utf-8")
        cls.picker = StubPicker(str(root / "out"))
        cls.srv, cls.port = create_server(root, port=0, picker=cls.picker,
                                          engine_cmd=["python", "-c", ""])
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.ctx.queue.stop()
        cls.srv.shutdown()
        cls.srv.server_close()
        cls.td.cleanup()

    def post(self, path, body=None):
        data = json.dumps(body or {}).encode("utf-8")
        req = urllib.request.Request(self.base + path, data=data, method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8") or "{}")

    def test_pick_outdir_returns_folder(self):
        status, obj = self.post("/api/pick/outdir")
        self.assertEqual(status, 200)
        self.assertEqual(obj["path"], self.picker.folder)
        self.assertEqual(self.picker.titles[-1], "选择输出目录")

    def test_jobs_start_reports_queued_count(self):
        status, obj = self.post("/api/jobs/start")
        self.assertEqual(status, 200)
        self.assertIn("queued", obj)
        self.assertIsInstance(obj["jobs"], list)


if __name__ == "__main__":
    unittest.main()
