"""引擎探测与引擎 stdout 解析（纯函数部分）。

对外契约（全队共用，勿改名；队列实现见文件末尾的 Task 4 分节）：

    detect_engine(repo_root: Path, override: str = "") -> str | None
    parse_progress_line(line: str) -> float | None
    parse_stats_line(line: str, stats: dict) -> bool
    sanitize_prefix(stem: str) -> str
    unique_prefix(output_dir: Path, stem: str) -> str

容错原则：
- 解析函数对**未知行**一律返回 ``None`` / ``False``，绝不抛异常：真引擎 stdout 会随版本增删行，
  界面必须容忍。
- 正则表照抄真实引擎输出（``STATS_PATTERNS``）。``RANGE观测帧`` 与 ``RANGE行`` 是两组独立正则，
  互不匹配对方（``SATVIS`` / ``SATVIS2`` 同理）。
- ``sanitize_prefix`` 先去掉扩展名再清洗非法字符（设计文档：输出前缀 = 输入文件名去掉扩展名
  并清洗非法字符）。
"""
import os, re
from pathlib import Path

# ---------- 纯函数：stdout 解析与探测 ----------

# 进度行形如 "  进度: 80% (320.00 MB / 395.55 MB)"；要求带括号，
# 避免把 "进度: 43%" 这类残缺行误判为有效进度。
PROGRESS_RE = re.compile(r"进度:\s*(\d+(?:\.\d+)?)\s*%\s*\(")

STATS_PATTERNS = [
    (re.compile(r"成功解析帧数:\s*(\d+)"), "totalFrames"),
    (re.compile(r"RANGE观测帧:\s*(\d+)"), "rangeFrames"),
    (re.compile(r"SATVIS可见性帧:\s*(\d+)"), "satvisFrames"),
    (re.compile(r"SATVIS2可见性帧:\s*(\d+)"), "satvis2Frames"),
    (re.compile(r"BESTPOS定位帧:\s*(\d+)"), "bestposFrames"),
    (re.compile(r"sync_lost\):\s*(\d+)"), "syncLost"),
    (re.compile(r"crc_error\):\s*(\d+)"), "crcErrors"),
    (re.compile(r"malformed\):\s*(\d+)"), "malformed"),
    (re.compile(r"unsupported\):\s*(\d+)"), "unsupported"),
    (re.compile(r"RANGE行:\s*(\d+)"), "rangeRows"),
    (re.compile(r"SATVIS行:\s*(\d+)"), "satvisRows"),
    (re.compile(r"SATVIS2行:\s*(\d+)"), "satvis2Rows"),
    (re.compile(r"BESTPOS行:\s*(\d+)"), "bestposRows"),
    (re.compile(r"CSV 写入错误:\s*(\d+)"), "csvWriteErrors"),
]


def parse_progress_line(line: str) -> float | None:
    """解析进度行，返回 0-100 的百分比；非进度行返回 ``None``。"""
    m = PROGRESS_RE.search(line)
    return float(m.group(1)) if m else None


def parse_stats_line(line: str, stats: dict) -> bool:
    """把命中的统计行写入 ``stats``（键为契约字段名），返回是否命中。"""
    for pattern, key in STATS_PATTERNS:
        m = pattern.search(line)
        if m:
            stats[key] = int(m.group(1))
            return True
    return False


# 非法字符（Windows 保留字符 + 路径分隔符 + 空白）连续出现时合并为一个下划线
_ILLEGAL_RE = re.compile(r'[\\/:*?"<>|\s]+')


def sanitize_prefix(stem: str) -> str:
    """文件名/主干 → 安全输出前缀：去扩展名，非法字符与空白替换为 ``_``。"""
    base = os.path.splitext(str(stem))[0]
    return _ILLEGAL_RE.sub("_", base)


# 引擎可能产出的 CSV 种类；判重时任一存在即视为前缀被占用
_CSV_KINDS = ("range", "satvis", "satvis2", "bestpos")

# 引擎默认前缀；实际产出 = f"{prefix}_{输入主名}_{类型}.csv"
DEFAULT_PREFIX = "gnss"


def unique_prefix(output_dir: Path, stem: str, taken=()) -> str:
    """返回未被占用的输出前缀（该值直接作为 -p 传给引擎）。

    真引擎实际产出 ``{prefix}_{stem}_{kind}.csv``（prefix = -p 值 + '_' + 输入主名），
    因此判重必须带上原始主名 ``stem``；``stem`` 应为输入文件的**原始主名**
    （``Path(input).stem``），不要传 sanitize 后的名字。

    ``taken`` 是**已在队列里但还没落盘**的同名文件集合：只查文件系统会让两次连续入队的
    同名输入拿到同一个前缀，第二个任务的结果覆盖第一个（见 Task 10 端到端测试）。
    """
    out = Path(output_dir)
    taken = set(taken)
    candidate, n = DEFAULT_PREFIX, 1
    while (any((out / f"{candidate}_{stem}_{kind}.csv").exists() for kind in _CSV_KINDS)
           or any(f"{candidate}_{stem}_{kind}.csv" in taken for kind in _CSV_KINDS)):
        n += 1
        candidate = f"{DEFAULT_PREFIX}-{n}"
    return candidate


# 引擎构建产物候选路径（相对仓库根），按优先级排列
BUILD_CANDIDATES = (("build", "Release", "gnss_parser.exe"), ("build", "gnss_parser.exe"))


def detect_engine(repo_root: Path, override: str = "") -> str | None:
    """探测引擎可执行文件：override → ``GNSS_PARSER_EXE`` → ``build`` 常见产物；均无则 ``None``。"""
    if override and Path(override).is_file():
        return str(Path(override))
    env = os.environ.get("GNSS_PARSER_EXE", "")
    if env and Path(env).is_file():
        return str(Path(env))
    for parts in BUILD_CANDIDATES:
        candidate = Path(repo_root).joinpath(*parts)
        if candidate.is_file():
            return str(candidate)
    return None


# ---------- 任务队列（Task 4）：JobQueue 与子进程/线程管理 ----------

import copy
import glob as _glob
import subprocess
import threading
import time
from datetime import datetime

# 仍在进行中的状态（取消只对这两种有意义）
RUNNING_STATES = ("queued", "running")

# stdout 中"输出文件"段的标题片段；该段之后每行一个绝对路径
_OUTPUT_SECTION_MARK = "输出文件"


def _now() -> str:
    """当前本地时间的 ISO 字符串（秒精度），供任务时间戳使用。"""
    return datetime.now().isoformat(timespec="seconds")


def new_stats() -> dict:
    """契约里 ``stats`` 的全字段初值（失败/取消时保留已解析到的值）。"""
    return {
        "totalFrames": 0, "rangeFrames": 0, "satvisFrames": 0, "satvis2Frames": 0,
        "bestposFrames": 0, "syncLost": 0, "crcErrors": 0, "malformed": 0,
        "unsupported": 0, "rangeRows": 0, "satvisRows": 0, "satvis2Rows": 0,
        "bestposRows": 0, "csvWriteErrors": 0, "outputFiles": [], "elapsedSec": 0.0,
        "exitCode": None,
    }


class JobQueue:
    """串行任务队列：单工作线程 + 单子进程，事件经 ``on_event`` 回调外发。

    状态机：``queued → running → done | failed | cancelled``；界面重启后残留的
    ``running`` 由 ``recover_interrupted()`` 改为 ``interrupted``（可重试）。

    与引擎的约定（实测）：``-p <prefix>`` 的实际产出是 ``{prefix}_{输入主名}_{类型}.csv``，
    因此 prefix 由 :func:`unique_prefix` 按原始主名判重后给出，可直接作为 ``-p`` 传入。
    """

    def __init__(self, state, repo_root, on_event, engine_cmd=None):
        self.state = state
        self.repo_root = Path(repo_root)
        self.on_event = on_event
        self._engine_cmd = list(engine_cmd) if engine_cmd else None
        self._lock = threading.RLock()
        self._jobs = [dict(j) for j in state.jobs]
        self._stop = threading.Event()
        self._worker = None
        self._proc = None
        self._current_id = None
        self._run_requested = False     # 手动"开始解析"用（autoStart 关闭时）
        self._seq = 0
        self._last_save = 0.0
        self._save_interval = 0.5

    # ---------- 生命周期 ----------

    def start(self) -> None:
        """启动工作线程（幂等）。"""
        if self._worker and self._worker.is_alive():
            return
        self._stop.clear()
        self._worker = threading.Thread(target=self._worker_loop, name="JobQueue", daemon=True)
        self._worker.start()

    def stop(self) -> None:
        """停止队列：终止当前子进程并等工作线程退出（≤5s）。"""
        self._stop.set()
        self._terminate_current()
        worker, self._worker = self._worker, None
        if worker and worker.is_alive():
            worker.join(timeout=5)

    def start_pending(self) -> int:
        """手动放行所有已排队任务（对应界面上 autoStart 关闭时点"开始解析"）。"""
        with self._lock:
            n = sum(1 for j in self._jobs if j["status"] == "queued")
        self._run_requested = True
        return n

    # ---------- 查询 ----------

    def snapshot(self) -> list[dict]:
        """返回任务列表的深拷贝（避免前端读到半更新对象）。"""
        with self._lock:
            return [copy.deepcopy(j) for j in self._jobs]

    def recover_interrupted(self) -> int:
        """把重启前残留的 ``running`` 任务标记为 ``interrupted``，返回改动条数。"""
        n = 0
        with self._lock:
            for job in self._jobs:
                if job.get("status") == "running":
                    job["status"] = "interrupted"
                    job["message"] = "界面重启前该任务仍在运行，已标记为中断（可重试）"
                    job["finishedAt"] = _now()
                    n += 1
        if n:
            self._save(force=True)
        return n

    # ---------- 入队与操作 ----------

    def enqueue(self, paths, output_dir, uploaded: bool = False) -> list[dict]:
        """把若干输入文件加入队列；预检不通过的任务直接标 ``failed`` 并附原因。"""
        out = Path(output_dir)
        engine = self._engine()
        created = []
        for raw in paths:
            path = Path(raw)
            job = self._make_job(path, out, uploaded)
            problem = self._preflight(path, out, engine)
            if problem:
                job["status"] = "failed"
                job["message"] = problem
                job["finishedAt"] = _now()
            # 状态为 queued 的任务由工作线程按列表顺序领取，无需另建待办队列
            with self._lock:
                self._jobs.append(job)
            created.append(dict(job))
            self._emit_job(job)
        self._save(force=True)
        return created

    def cancel(self, job_id: str) -> bool:
        """取消排队中或运行中的任务；返回是否生效。"""
        with self._lock:
            job = self._find(job_id)
            if job is None or job["status"] not in RUNNING_STATES:
                return False
            if job["status"] == "queued":
                job["status"] = "cancelled"
                job["finishedAt"] = _now()
                self._emit_job(job)
                self._save(force=True)
                return True
        self._terminate_current()
        with self._lock:
            job = self._find(job_id)
            if job is None:
                return False
            job["status"] = "cancelled"
            job["finishedAt"] = _now()
            self._emit_job(job)
        self._save(force=True)
        return True

    def retry(self, job_id: str) -> bool:
        """重试失败/取消/中断的任务：重算 prefix（绝不覆盖旧产物）后重新排队。"""
        with self._lock:
            job = self._find(job_id)
            if job is None or job["status"] not in ("failed", "cancelled", "interrupted"):
                return False
            job["prefix"] = unique_prefix(Path(job["outputDir"]), Path(job["inputPath"]).stem,
                                          self._taken_names(job))
            job["status"] = "queued"
            job["progress"] = 0.0
            job["message"] = ""
            job["stats"] = new_stats()
            job["queuedAt"] = _now()
            job["startedAt"] = ""
            job["finishedAt"] = ""
        self._emit_job(job)
        self._save(force=True)
        return True

    def remove(self, job_id: str) -> bool:
        """删除一个非运行中的任务。"""
        with self._lock:
            job = self._find(job_id)
            if job is None or job["status"] in RUNNING_STATES:
                return False
            self._jobs.remove(job)
        self._save(force=True)
        return True

    def clear(self, keep_running: bool = True) -> int:
        """清空任务列表；``keep_running`` 为真时保留排队中/运行中的任务。"""
        with self._lock:
            if keep_running:
                keep = [j for j in self._jobs if j["status"] in RUNNING_STATES]
            else:
                keep = []
            removed = len(self._jobs) - len(keep)
            self._jobs = keep
        if removed:
            self._save(force=True)
        return removed

    # ---------- 内部：任务执行 ----------

    def _worker_loop(self) -> None:
        """串行取活：**在同一把锁里**判定"是否放行"与"取哪个任务"，顺序即列表顺序（自上而下）。

        早期实现把放行判定放在阻塞等待任务队列之前，导致"关掉『拖入即开始』后刚入队的任务
        仍会执行"（判定通过后 worker 已阻塞在取任务上，设置变更无法阻止它）。改为直接扫描
        任务列表后，放行开关与任务选取是原子的，也不再有"待办 id 队列与任务列表不一致"的问题
        （删除/清空任务后残留 id 的旧坑一并消失）。
        """
        while not self._stop.is_set():
            job = None
            with self._lock:
                allowed = bool(self.state.settings.get("autoStart", True)) or self._run_requested
                if allowed:
                    job = next((j for j in self._jobs if j["status"] == "queued"), None)
                if job is None:
                    self._run_requested = False
            if job is None:
                time.sleep(0.05)
                continue
            self._run_job(job)

    def _run_job(self, job: dict) -> None:
        engine = self._engine()
        if not engine:
            self._finish_with_error(job, "未找到解析引擎 gnss_parser.exe，请在界面顶部指定引擎路径")
            return
        with self._lock:
            job["status"] = "running"
            job["startedAt"] = _now()
            job["progress"] = 0.0
            job["message"] = ""
            job["stats"] = new_stats()
            self._current_id = job["id"]
        self._emit_job(job)
        self._save(force=True)

        cmd = (list(engine) if isinstance(engine, (list, tuple)) else [str(engine)])
        cmd += ["-i", job["inputPath"], "-o", job["outputDir"], "-p", job["prefix"]]
        started = time.monotonic()
        tail: list[str] = []
        output_lines: list[str] = []
        in_output_section = False
        last_pct = -1
        try:
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            self._proc = subprocess.Popen(
                cmd, cwd=str(self.repo_root), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", creationflags=flags)
        except OSError as exc:
            self._finish_with_error(job, f"无法启动引擎: {exc}", started)
            return

        assert self._proc.stdout is not None
        for line in self._proc.stdout:
            line = line.rstrip("\r\n")
            if line.strip():
                tail.append(line.strip())
                if len(tail) > 5:
                    tail.pop(0)
            if _OUTPUT_SECTION_MARK in line:
                in_output_section = True
                continue
            if in_output_section:
                stripped = line.strip()
                if stripped.lower().endswith(".csv"):
                    output_lines.append(stripped)
                    continue
                if stripped:
                    in_output_section = False
            pct = parse_progress_line(line)
            if pct is not None:
                job["progress"] = pct
                if int(pct) != last_pct:
                    last_pct = int(pct)
                    self._emit_progress(job)
                    self._save()
            parse_stats_line(line, job["stats"])

        rc = self._proc.wait()
        if self._proc.stdout is not None:
            self._proc.stdout.close()      # 显式关闭管道，避免 ResourceWarning / 句柄泄漏
        self._proc = None
        with self._lock:
            self._current_id = None
        job["stats"]["exitCode"] = rc
        job["stats"]["elapsedSec"] = round(time.monotonic() - started, 1)
        if not output_lines:
            pattern = f"{job['prefix']}_{Path(job['inputPath']).stem}_*.csv"
            output_lines = sorted(_glob.glob(str(Path(job["outputDir"]) / pattern)))
        job["stats"]["outputFiles"] = output_lines

        if job["status"] != "cancelled":
            if rc == 0 and job["stats"]["totalFrames"] > 0:
                job["status"] = "done"
                job["progress"] = 100.0
            else:
                job["status"] = "failed"
                reason = next((t for t in tail if "有效帧" in t or "解析失败" in t), "")
                parts = [f"引擎退出码 {rc}"]
                if reason:
                    parts.append(reason)
                elif tail:
                    parts.append(tail[-1])
                job["message"] = "；".join(parts)
        job["finishedAt"] = _now()
        self._emit_job(job)
        self._save(force=True)

    def _finish_with_error(self, job: dict, message: str, started: float | None = None) -> None:
        with self._lock:
            self._proc = None
            self._current_id = None
            job["status"] = "failed"
            job["message"] = message
            job["finishedAt"] = _now()
            if started is not None:
                job["stats"]["elapsedSec"] = round(time.monotonic() - started, 1)
        self._emit_job(job)
        self._save(force=True)

    def _terminate_current(self) -> None:
        proc = self._proc
        if proc is None or proc.poll() is not None:
            return
        try:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        except OSError:
            pass

    # ---------- 内部：任务装配与状态 ----------

    def _taken_names(self, exclude: dict | None = None) -> set:
        """队列里其它任务**将要产出**的 CSV 文件名集合（用于前缀判重，避免互相覆盖）。"""
        taken = set()
        for job in self._jobs:
            if exclude is not None and job["id"] == exclude["id"]:
                continue
            stem = Path(job["inputPath"]).stem
            for kind in _CSV_KINDS:
                taken.add(f"{job['prefix']}_{stem}_{kind}.csv")
        return taken

    def _make_job(self, path: Path, out_dir: Path, uploaded: bool) -> dict:
        self._seq += 1
        job_id = f"j{datetime.now():%Y%m%d%H%M%S}-{self._seq:03d}"
        size = path.stat().st_size if path.is_file() else 0
        return {
            "id": job_id,
            "inputPath": str(path),
            "inputName": path.name,
            "sizeBytes": size,
            "isUploaded": uploaded,
            "outputDir": str(out_dir),
            "prefix": unique_prefix(out_dir, path.stem, self._taken_names()),
            "status": "queued",
            "progress": 0.0,
            "message": "",
            "warnings": [],
            "stats": new_stats(),
            "queuedAt": _now(),
            "startedAt": "",
            "finishedAt": "",
        }

    def _preflight(self, path: Path, out_dir: Path, engine) -> str:
        """预检；返回空串表示可通过，否则返回给用户看的原因。"""
        if not engine:
            return "未找到解析引擎 gnss_parser.exe，请在界面顶部指定引擎路径"
        if not path.is_file():
            return f"输入文件不存在: {path}"
        if path.stat().st_size == 0:
            return "输入文件为空（0 字节）"
        try:
            out_dir.mkdir(parents=True, exist_ok=True)
            probe = out_dir / ".gnss_gui_write_probe"
            probe.write_text("ok", encoding="utf-8")
            probe.unlink()
        except OSError as exc:
            return f"输出目录不可写: {exc}"
        return ""

    def _engine(self):
        """当前引擎命令：显式传入的 engine_cmd 优先，否则按设置/构建产物探测。"""
        if self._engine_cmd:
            return self._engine_cmd
        return detect_engine(self.repo_root, self.state.settings.get("enginePath", ""))

    def _find(self, job_id: str) -> dict | None:
        return next((j for j in self._jobs if j["id"] == job_id), None)

    # ---------- 内部：事件与持久化 ----------

    def _emit(self, event: dict) -> None:
        try:
            self.on_event(event)
        except Exception:
            pass

    def _emit_job(self, job: dict) -> None:
        self._emit({"type": "job", "job": copy.deepcopy(job)})

    def _emit_progress(self, job: dict) -> None:
        self._emit({"type": "progress", "id": job["id"], "progress": job["progress"],
                    "stats": dict(job["stats"])})

    def _save(self, force: bool = False) -> None:
        """节流写盘（默认 0.5s）；写失败不能拖垮队列，原因留在 state.last_error。"""
        now = time.monotonic()
        if not force and now - self._last_save < self._save_interval:
            return
        self._last_save = now
        try:
            self.state.set_jobs(self.snapshot())
            self.state.save()
        except Exception:
            pass
