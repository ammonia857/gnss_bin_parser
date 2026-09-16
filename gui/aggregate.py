"""CSV 流式聚合：统计概览、行索引与分页（供界面结果页使用）。

引擎 CSV 的实测格式：**UTF-8 BOM** 开头、**CRLF** 行尾、可达数百万行，
因此一律流式处理，绝不整体读入内存。

约定：``build_row_index`` 的 ``every`` 必须与 :data:`INDEX_EVERY` 一致（默认 5000），
``page_rows`` 依赖该常量把"数据行号"映射到索引块。
"""
import csv
from pathlib import Path

# 行索引的块大小（数据行数/块）；build_row_index 与 page_rows 必须一致
INDEX_EVERY = 5000


def _bom(header_field: str) -> str:
    """去掉可能残留的 BOM（二进制读时 BOM 会落在首个字段里）。"""
    return header_field.lstrip("\ufeff")


def summarize(csv_path, cache=None) -> dict:
    """流式扫描一个 CSV，返回行数 / 坏行数 / 星座分布 / 时间跨度 / 列名。

    坏行（字段数与表头不一致）只计数不抛异常；表头缺少 ``Sat_System`` /
    ``Sat_System_Name`` / ``TOW_ms`` 时对应项为空 dict / 空 dict / ``None``。
    """
    path = Path(csv_path)
    if cache is not None:
        hit = cache.get(path)
        if hit is not None:
            return hit

    summary = {"rows": 0, "skippedRows": 0, "systems": {}, "systemNames": {},
               "timeSpan": None, "columns": []}
    try:
        handle = open(path, "r", encoding="utf-8-sig", newline="")
    except OSError:
        if cache is not None:
            cache.put(path, summary)
        return summary

    with handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header:
            if cache is not None:
                cache.put(path, summary)
            return summary
        columns = [_bom(c) for c in header]
        summary["columns"] = columns
        try:
            sys_idx = columns.index("Sat_System")
        except ValueError:
            sys_idx = None
        try:
            name_idx = columns.index("Sat_System_Name")
        except ValueError:
            name_idx = None
        try:
            tow_idx = columns.index("TOW_ms")
        except ValueError:
            tow_idx = None

        n_cols = len(columns)
        rows = skipped = 0
        systems: dict = {}
        names: dict = {}
        tow_min = tow_max = None
        for row in reader:
            if len(row) != n_cols:
                skipped += 1
                continue
            rows += 1
            if sys_idx is not None:
                key = row[sys_idx]
                systems[key] = systems.get(key, 0) + 1
            if name_idx is not None:
                key = row[name_idx]
                names[key] = names.get(key, 0) + 1
            if tow_idx is not None:
                raw = row[tow_idx]
                if raw:
                    try:
                        value = int(raw)
                    except ValueError:
                        value = None
                    if value is not None:
                        if tow_min is None or value < tow_min:
                            tow_min = value
                        if tow_max is None or value > tow_max:
                            tow_max = value

        summary["rows"] = rows
        summary["skippedRows"] = skipped
        summary["systems"] = systems
        summary["systemNames"] = names
        if tow_min is not None:
            summary["timeSpan"] = {"towMin": tow_min, "towMax": tow_max}

    if cache is not None:
        cache.put(path, summary)
    return summary


class SummaryCache:
    """按 (路径, 文件大小, mtime_ns) 缓存 summarize 结果（文件变化即失效）。"""

    def __init__(self):
        self._items: dict = {}

    @staticmethod
    def _key(csv_path: Path):
        st = Path(csv_path).stat()
        return (str(csv_path), st.st_size, st.st_mtime_ns)

    def get(self, csv_path):
        try:
            return self._items.get(self._key(csv_path))
        except OSError:
            return None

    def put(self, csv_path: Path, summary: dict) -> None:
        try:
            self._items[self._key(csv_path)] = summary
        except OSError:
            pass


def build_row_index(csv_path: Path, every: int = INDEX_EVERY) -> list[int]:
    """返回每 ``every`` 条数据行首行的**字节偏移**；``index[0]`` 是第一条数据行。"""
    offsets: list[int] = []
    with open(csv_path, "rb") as handle:
        header = handle.readline()
        if not header:
            return []
        offsets.append(len(header))
        count = 0
        while True:
            line = handle.readline()
            if not line:
                break
            count += 1
            if count % every == 0:
                offsets.append(handle.tell())
    return offsets


def page_rows(csv_path: Path, index: list[int], offset: int, limit: int,
              filter_system: int | None = None) -> list[dict]:
    """按数据行号 ``offset`` 分页读取（0 基），最多 ``limit`` 行。

    ``filter_system`` 给出时只返回 ``Sat_System == str(filter_system)`` 的行；
    越界返回 ``[]``。二进制读 + 按行解码，避免文本模式下 seek 到半个字符。
    """
    if not index or limit <= 0 or offset < 0:
        return []
    block = min(offset // INDEX_EVERY, len(index) - 1)
    start = index[block]
    skip = offset - block * INDEX_EVERY
    want = None if filter_system is None else str(filter_system)

    with open(csv_path, "rb") as handle:
        header_line = handle.readline()
        if not header_line:
            return []
        header = next(csv.reader([header_line.decode("utf-8", "replace")]))
        header = [_bom(c) for c in header]
        try:
            sys_idx = header.index("Sat_System")
        except ValueError:
            sys_idx = None

        handle.seek(start)
        for _ in range(skip):
            if not handle.readline():
                return []

        out: list[dict] = []
        while len(out) < limit:
            line = handle.readline()
            if not line:
                break
            fields = next(csv.reader([line.decode("utf-8", "replace")]))
            if len(fields) != len(header):
                continue
            if want is not None and (sys_idx is None or fields[sys_idx] != want):
                continue
            out.append(dict(zip(header, fields)))
        return out
