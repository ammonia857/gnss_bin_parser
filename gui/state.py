"""GUI 运行时状态：设置与任务队列记忆（持久化到 state.json）。

文件形状（全队共用契约，勿改名）：

    {"settings": {...}, "jobs": [ {...}, ... ]}

容错原则：
- 文件不存在 / JSON 损坏 / 顶层或字段类型不对 → 一律回退 DEFAULTS，原因写入 ``last_error``，
  ``load()`` 永不抛异常（界面必须能带着默认值起来）。
- ``save()`` 原子写：先写 ``<name>.tmp`` 再 ``os.replace``，避免半截 JSON 覆盖掉可用状态。
- 设置项按白名单过滤，只接受 ``DEFAULTS`` 里已有的键。
"""
import copy, json, os
from pathlib import Path

DEFAULTS = {
    "enginePath": "",        # 空 = 自动探测
    "outputDir": "",         # 空 = <仓库根>/parsed_output
    "autoStart": True,
    "cleanupUploads": True,
    "port": 8765,
}

# 任务对象字段（dict 的键序）
JOB_FIELDS = ("id", "inputPath", "inputName", "sizeBytes", "isUploaded", "outputDir", "prefix",
              "status", "progress", "message", "warnings", "stats", "queuedAt", "startedAt",
              "finishedAt")
# status ∈ queued|running|done|failed|cancelled|interrupted


class State:
    """设置 + 任务列表的 JSON 持久化。构造时即加载，``load()`` 可重复调用。"""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.last_error: str | None = None
        self._settings = copy.deepcopy(DEFAULTS)
        self._jobs: list[dict] = []
        self.load()

    # ---------- 读写 ----------

    def load(self) -> None:
        """从磁盘加载；任何异常都回退默认值并记录原因，不向外抛。"""
        self.last_error = None
        self._settings = copy.deepcopy(DEFAULTS)   # 深拷贝：DEFAULTS 本身绝不被写脏
        self._jobs = []
        if not self.path.exists():
            return
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except Exception as exc:                   # JSON 语法错误 / 编码错误 / 读取失败
            self.last_error = f"状态文件无法解析，已回退默认值: {exc}"
            return
        if not isinstance(raw, dict):
            self.last_error = "状态文件顶层不是对象，已回退默认值"
            return

        settings = raw.get("settings", {})
        if isinstance(settings, dict):
            for key, value in settings.items():
                if key in DEFAULTS:                # 白名单：未知键丢弃（可能是旧版本残留）
                    self._settings[key] = value
        elif settings is not None:
            self.last_error = "settings 不是对象，已使用默认设置"

        jobs = raw.get("jobs", [])
        if isinstance(jobs, list) and all(isinstance(j, dict) for j in jobs):
            self._jobs = [dict(j) for j in jobs]
        elif jobs:
            self.last_error = "jobs 不是对象列表，已忽略"

    def save(self) -> None:
        """原子写：临时文件 + os.replace；成功路径下不留临时文件。"""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {"settings": self._settings, "jobs": self._jobs}
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        tmp = self.path.with_name(self.path.name + ".tmp")
        try:
            with open(tmp, "w", encoding="utf-8") as fh:
                fh.write(text)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)             # 同目录替换，POSIX/NT 均为原子
        except OSError as exc:
            self.last_error = f"状态保存失败: {exc}"
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
            raise

    # ---------- 访问器 ----------

    @property
    def settings(self) -> dict:
        return self._settings

    @property
    def jobs(self) -> list[dict]:
        return self._jobs

    def set_settings(self, patch: dict) -> dict:
        """按白名单合并设置项，返回更新后的完整 settings。"""
        for key, value in patch.items():
            if key in DEFAULTS:
                self._settings[key] = value
        return self._settings

    def set_jobs(self, jobs: list[dict]) -> None:
        """整体替换任务列表，字段原样保留（浅拷贝外层列表）。"""
        self._jobs = list(jobs)
