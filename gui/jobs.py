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


def unique_prefix(output_dir: Path, stem: str) -> str:
    """返回未被占用的输出前缀：``stem``、``stem-2``、``stem-3``…（绝不覆盖已存在 CSV）。"""
    out = Path(output_dir)
    candidate, n = stem, 1
    while any((out / f"{candidate}_{kind}.csv").exists() for kind in _CSV_KINDS):
        n += 1
        candidate = f"{stem}-{n}"
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


# ---------- 任务队列（Task 4）：JobQueue 及其子进程/线程管理在此分节追加 ----------
