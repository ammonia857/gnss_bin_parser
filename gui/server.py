"""本地网页界面后端：HTTP + SSE + 静态文件，仅监听 127.0.0.1。

职责边界：
- 本模块只管「协议层」：路由、JSON、SSE、静态文件、以及在资源管理器中打开路径；
- 任务调度在 :mod:`gui.jobs`，状态持久化在 :mod:`gui.state`，
  CSV 聚合在 :mod:`gui.aggregate`，原生对话框在 :mod:`gui.native_picker`。

P1 暴露的接口：settings / pick / upload / jobs（增查与取消重试删除清空）/ events(SSE) /
results.summary / results.rows / open。
"""
import argparse
import json
import mimetypes
import os
import queue
import re
import sys
import threading
import urllib.parse
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import aggregate
from .jobs import JobQueue
from .native_picker import NativePicker
from .state import State

JSON_BODY_LIMIT = 1 << 20          # 1MB：JSON 接口体上限
UPLOAD_CHUNK = 1 << 20             # 上传落盘分块
_JOB_ACTION_RE = re.compile(r"^/api/jobs/([^/]+)/(cancel|retry|remove)$")
_JOB_RESULT_RE = re.compile(r"^/api/results/([^/]+)/(summary|rows)$")
_MAX_PORT_TRIES = 16


class Context:
    """服务运行期共享状态（挂在 HTTPServer 实例上，供 Handler 取用）。"""

    def __init__(self, repo_root: Path, state: State, engine_cmd=None, picker=None):
        self.repo_root = Path(repo_root)
        self.static_dir = self.repo_root / "gui" / "static"
        self.state = state
        self.cache = aggregate.SummaryCache()
        self.row_index: dict = {}
        self.subscribers: set = set()
        self.subscribers_lock = threading.Lock()
        self.picker = picker or NativePicker()
        self.queue = JobQueue(state, self.repo_root, self.broadcast, engine_cmd=engine_cmd)
        self.queue.recover_interrupted()

    # ---------- 事件广播 ----------

    def broadcast(self, event: dict) -> None:
        with self.subscribers_lock:
            targets = list(self.subscribers)
        for sub in targets:
            try:
                sub.put_nowait(event)
            except queue.Full:
                pass

    # ---------- 工具 ----------

    @property
    def output_dir(self) -> Path:
        configured = self.state.settings.get("outputDir") or ""
        return Path(configured) if configured else (self.repo_root / "parsed_output")

    def find_job(self, job_id: str):
        return next((j for j in self.queue.snapshot() if j["id"] == job_id), None)

    def output_files(self, job: dict) -> list[Path]:
        """任务的输出 CSV：优先用引擎 stdout 里"输出文件"段给出的路径，否则按命名规则回退 glob。"""
        files = [Path(p) for p in job.get("stats", {}).get("outputFiles", [])]
        files = [p for p in files if p.is_file()]
        if files:
            return files
        stem = Path(job["inputPath"]).stem
        return sorted(Path(job["outputDir"]).glob(f"{job['prefix']}_{stem}_*.csv"))

    def row_index_for(self, path: Path) -> list[int]:
        st = path.stat()
        key = str(path)
        cached = self.row_index.get(key)
        if cached and cached[0] == (st.st_size, st.st_mtime_ns):
            return cached[1]
        idx = aggregate.build_row_index(path)
        self.row_index[key] = ((st.st_size, st.st_mtime_ns), idx)
        return idx


def _dataset_kind(name: str) -> str:
    """`gnss_x_range.csv` → `range`；未知返回文件名主干。"""
    stem = Path(name).stem
    for kind in ("satvis2", "satvis", "range", "bestpos"):
        if stem.endswith(f"_{kind}"):
            return kind
    return stem


def create_server(repo_root, port: int = 8765, engine_cmd=None, picker=None):
    """创建并返回 ``(ThreadingHTTPServer, actual_port)``；``port=0`` 时由系统分配。"""
    ctx = Context(Path(repo_root), _load_state(Path(repo_root)), engine_cmd=engine_cmd, picker=picker)
    handler = _make_handler(ctx)
    candidates = [0] if port == 0 else list(range(port, port + _MAX_PORT_TRIES))
    last_error = None
    for candidate in candidates:
        try:
            server = ThreadingHTTPServer(("127.0.0.1", candidate), handler)
        except OSError as exc:                      # 端口占用 → 试下一个
            last_error = exc
            continue
        server.ctx = ctx
        server.daemon_threads = True
        ctx.queue.start()
        return server, server.server_address[1]
    raise OSError(f"无法在 {port}~{port + _MAX_PORT_TRIES - 1} 范围内监听端口: {last_error}")


def _load_state(repo_root: Path) -> State:
    state = State(repo_root / "gui" / "state.json")
    state.load()
    return state


def _make_handler(ctx: Context):
    class Handler(BaseHTTPRequestHandler):
        server_version = "gnss-gui/0.1"
        protocol_version = "HTTP/1.1"

        # ---------- 基础工具 ----------

        def log_message(self, fmt, *args):          # 静音默认访问日志（避免刷屏）
            pass

        def _send_json(self, status: int, payload) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _error(self, status: int, message: str) -> None:
            self._send_json(status, {"error": message})

        def _read_json(self) -> dict:
            length = int(self.headers.get("Content-Length") or 0)
            if length <= 0:
                return {}
            if length > JSON_BODY_LIMIT:
                raise ValueError("请求体过大")
            raw = self.rfile.read(length)
            if not raw:
                return {}
            return json.loads(raw.decode("utf-8"))

        def _query(self) -> dict:
            parsed = urllib.parse.urlparse(self.path)
            return {k: v[0] for k, v in urllib.parse.parse_qs(parsed.query).items()}

        def _route(self) -> str:
            return urllib.parse.urlparse(self.path).path

        # ---------- 路由 ----------

        def do_GET(self):                            # noqa: N802（http.server 约定）
            try:
                path = self._route()
                if path == "/":
                    return self._serve_static("index.html")
                if path.startswith("/static/"):
                    return self._serve_static(path[len("/static/"):])
                if path == "/api/settings":
                    return self._send_json(200, ctx.state.settings)
                if path == "/api/jobs":
                    return self._send_json(200, {"jobs": ctx.queue.snapshot()})
                if path == "/api/events":
                    return self._serve_sse()
                m = _JOB_RESULT_RE.match(path)
                if m:
                    return self._result(m.group(1), m.group(2))
                return self._error(404, f"未知路径: {path}")
            except Exception as exc:                 # 兜底：任何异常都回 500，不让线程崩
                return self._error(500, f"{type(exc).__name__}: {exc}")

        def do_POST(self):                           # noqa: N802
            try:
                path = self._route()
                if path == "/api/settings":
                    patch = self._read_json()
                    ctx.state.set_settings(patch if isinstance(patch, dict) else {})
                    ctx.state.save()
                    return self._send_json(200, ctx.state.settings)
                if path == "/api/pick/files":
                    return self._send_json(200, {"paths": ctx.picker.pick_files() or []})
                if path == "/api/pick/folder":
                    folder = ctx.picker.pick_folder()
                    if not folder:
                        return self._send_json(200, {"paths": []})
                    bins = sorted(str(p) for p in Path(folder).glob("*.bin"))
                    return self._send_json(200, {"paths": bins})
                if path == "/api/pick/outdir":
                    return self._send_json(200, {"path": ctx.picker.pick_folder(title="选择输出目录") or ""})
                if path == "/api/upload":
                    return self._upload()
                if path == "/api/jobs":
                    return self._create_jobs()
                if path == "/api/jobs/clear":
                    removed = ctx.queue.clear(keep_running=True)
                    return self._send_json(200, {"removed": removed})
                if path == "/api/jobs/start":
                    queued = ctx.queue.start_pending()
                    return self._send_json(200, {"queued": queued, "jobs": ctx.queue.snapshot()})
                m = _JOB_ACTION_RE.match(path)
                if m:
                    return self._job_action(m.group(1), m.group(2))
                if path == "/api/open":
                    return self._open_path()
                return self._error(404, f"未知路径: {path}")
            except Exception as exc:
                return self._error(500, f"{type(exc).__name__}: {exc}")

        # ---------- 静态文件 ----------

        def _serve_static(self, rel: str) -> None:
            target = (ctx.static_dir / rel).resolve()
            if not str(target).startswith(str(ctx.static_dir.resolve())) or not target.is_file():
                return self._error(404, f"静态文件不存在: {rel}")
            body = target.read_bytes()
            ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
            if ctype.startswith("text/") or ctype in ("application/javascript",):
                ctype += "; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        # ---------- SSE ----------

        def _serve_sse(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            channel: "queue.Queue" = queue.Queue(maxsize=2000)
            with ctx.subscribers_lock:
                ctx.subscribers.add(channel)
            try:
                self._sse_write({"type": "hello", "jobs": ctx.queue.snapshot(),
                                 "settings": ctx.state.settings})
                while True:
                    try:
                        event = channel.get(timeout=15)
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")     # 心跳，防中间层断开
                        self.wfile.flush()
                        continue
                    self._sse_write(event)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass
            finally:
                with ctx.subscribers_lock:
                    ctx.subscribers.discard(channel)

        def _sse_write(self, event: dict) -> None:
            payload = json.dumps(event, ensure_ascii=False).replace("\n", " ")
            self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
            self.wfile.flush()

        # ---------- 任务 ----------

        def _create_jobs(self) -> None:
            body = self._read_json()
            paths = body.get("paths") or []
            if not isinstance(paths, list) or not paths:
                return self._error(400, "paths 不能为空")
            out_dir = body.get("outputDir") or str(ctx.output_dir)
            if body.get("outputDir"):
                ctx.state.set_settings({"outputDir": body["outputDir"]})
                ctx.state.save()
            jobs = ctx.queue.enqueue([str(p) for p in paths], out_dir, uploaded=bool(body.get("uploaded")))
            return self._send_json(200, {"jobs": jobs})

        def _job_action(self, job_id: str, action: str) -> None:
            if action == "cancel":
                ok = ctx.queue.cancel(job_id)
            elif action == "retry":
                ok = ctx.queue.retry(job_id)
            else:
                ok = ctx.queue.remove(job_id)
            if not ok:
                return self._error(409, f"任务 {job_id} 当前状态不允许 {action}")
            return self._send_json(200, {"ok": True, "jobs": ctx.queue.snapshot()})

        # ---------- 上传（拖拽兜底） ----------

        def _upload(self) -> None:
            length = int(self.headers.get("Content-Length") or 0)
            if length <= 0:
                return self._error(411, "缺少 Content-Length")
            raw_name = urllib.parse.unquote(self.headers.get("X-Filename") or "upload.bin")
            safe_name = Path(raw_name).name or "upload.bin"
            upload_dir = Path(os.environ.get("TEMP", "/tmp")) / "gnss_gui_uploads"
            upload_dir.mkdir(parents=True, exist_ok=True)
            target = upload_dir / f"{uuid.uuid4().hex[:8]}_{safe_name}"
            remaining = length
            with open(target, "wb") as handle:
                while remaining > 0:
                    chunk = self.rfile.read(min(UPLOAD_CHUNK, remaining))
                    if not chunk:
                        break
                    handle.write(chunk)
                    remaining -= len(chunk)
            return self._send_json(200, {"path": str(target), "name": safe_name, "sizeBytes": length})

        # ---------- 结果聚合 ----------

        def _result(self, job_id: str, kind: str) -> None:
            job = ctx.find_job(job_id)
            if job is None:
                return self._error(404, f"任务不存在: {job_id}")
            files = ctx.output_files(job)
            if kind == "summary":
                datasets = {}
                for path in files:
                    datasets[_dataset_kind(path.name)] = aggregate.summarize(path, ctx.cache)
                total = sum(d.get("rows", 0) for d in datasets.values())
                return self._send_json(200, {
                    "files": [{"name": p.name, "path": str(p), "sizeBytes": p.stat().st_size}
                              for p in files],
                    "datasets": datasets,
                    "totalRows": total,
                })
            # rows：分页
            dataset = self._query().get("dataset", "range")
            target = next((p for p in files if _dataset_kind(p.name) == dataset), None)
            if target is None:
                return self._error(404, f"任务 {job_id} 没有 {dataset} 数据集")
            query = self._query()
            offset = int(query.get("offset", 0) or 0)
            limit = min(int(query.get("limit", 100) or 100), 1000)
            filter_system = query.get("filterSystem")
            rows = aggregate.page_rows(target, ctx.row_index_for(target), offset, limit,
                                       filter_system=int(filter_system) if filter_system not in (None, "") else None)
            return self._send_json(200, {"dataset": dataset, "offset": offset, "rows": rows})

        # ---------- 打开目录/文件 ----------

        def _open_path(self) -> None:
            body = self._read_json()
            raw = body.get("path") or ""
            if not raw:
                return self._error(400, "缺少 path")
            target = Path(raw)
            allowed_dirs = {Path(j["outputDir"]).resolve() for j in ctx.queue.snapshot()}
            try:
                resolved = target.resolve()
            except OSError:
                return self._error(400, f"路径无效: {raw}")
            inside = any(str(resolved).startswith(str(d)) for d in allowed_dirs)
            if not target.exists() or not inside:
                return self._error(403, "只允许打开本工具输出目录内的路径")
            try:
                if sys.platform == "win32":
                    os.startfile(str(resolved))                     # noqa: S606（仅本机 GUI）
                elif sys.platform == "darwin":
                    os.system(f'open "{resolved}"')                 # noqa: S605
                else:
                    os.system(f'xdg-open "{resolved}"')             # noqa: S605
            except OSError as exc:
                return self._error(500, f"打开失败: {exc}")
            return self._send_json(200, {"ok": True})

    return Handler


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="GNSS 解析工具本地网页界面")
    parser.add_argument("--port", type=int, default=8765, help="监听端口（默认 8765，占用则自动 +1）")
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]), help="仓库根目录")
    parser.add_argument("--no-browser", action="store_true", help="启动后不自动打开浏览器")
    args = parser.parse_args(argv)

    server, port = create_server(Path(args.repo), port=args.port)
    url = f"http://127.0.0.1:{port}/"
    print(f"界面已启动: {url}")
    print("按 Ctrl+C 结束。")
    threading.Thread(target=server.serve_forever, name="http", daemon=True).start()
    if not args.no_browser:
        webbrowser.open(url)

    picker = server.ctx.picker
    try:
        picker.run_forever()                 # tkinter 必须在主线程
    except Exception as exc:                 # 无 GUI 环境（如无桌面会话）时退化为等待
        print(f"原生对话框不可用（{exc}），仍可拖拽上传或在非受控环境使用 API。")
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
    except KeyboardInterrupt:
        pass
    finally:
        server.ctx.queue.stop()
        server.shutdown()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
