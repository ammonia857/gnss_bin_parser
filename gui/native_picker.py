"""tkinter 原生文件对话框桥。

为什么需要它：浏览器拖拽拿不到本地真实路径（安全限制），而 400MB 级数据不该走上传，
因此由后端弹 **Windows 原生对话框**拿绝对路径。约束是 tkinter 的对话框必须在**主线程**
的事件循环里运行，所以本模块提供：

- :meth:`NativePicker.run_forever`：在主线程常驻，轮询请求队列并执行对话框；
- :meth:`NativePicker.pick_files` / :meth:`pick_folder`：供 HTTP 线程调用（阻塞等待结果，带超时）；
- ``dialog_files`` / ``dialog_folder`` 可注入假实现，便于在无 UI 的测试里验证请求/响应管线。
"""
import queue
import threading
from pathlib import Path


def _real_pick_files() -> list[str]:
    """真实"选择 .bin（可多选）"对话框。"""
    from tkinter import filedialog

    paths = filedialog.askopenfilenames(
        title="选择 BIN 文件",
        filetypes=[("NovAtel BIN", "*.bin"), ("所有文件", "*.*")])
    return [str(Path(p)) for p in paths]


def _real_pick_folder() -> str | None:
    """真实"选择文件夹"对话框。"""
    from tkinter import filedialog

    picked = filedialog.askdirectory(title="选择包含 BIN 的文件夹")
    return str(Path(picked)) if picked else None


class NativePicker:
    """原生对话框桥：请求队列 + 主线程事件循环。"""

    def __init__(self, dialog_files=None, dialog_folder=None):
        self._dialog_files = dialog_files or _real_pick_files
        self._dialog_folder = dialog_folder or _real_pick_folder
        self._requests: "queue.Queue[tuple[str, queue.Queue]]" = queue.Queue()
        self._stop = threading.Event()
        self._root = None

    # ---------- 主线程侧 ----------

    def run_forever(self) -> None:
        """在主线程常驻：创建隐藏 Tk 根窗口，循环处理请求直到 :meth:`stop`。"""
        import tkinter as tk

        root = tk.Tk()
        root.withdraw()
        self._root = root
        self._stop.clear()
        try:
            while not self._stop.is_set():
                try:
                    kind, reply = self._requests.get(timeout=0.05)
                except queue.Empty:
                    root.update()          # 维持窗口消息循环（对话框依赖它）
                    continue
                try:
                    if kind == "files":
                        reply.put(self._dialog_files())
                    elif kind == "folder":
                        reply.put(self._dialog_folder())
                    else:
                        reply.put(None)
                except Exception:          # 对话框异常不应打死常驻线程
                    reply.put(None)
                root.update()
        finally:
            try:
                root.destroy()
            except Exception:
                pass
            self._root = None

    def stop(self) -> None:
        """请求常驻线程退出（线程会在下一轮轮询时结束并销毁根窗口）。"""
        self._stop.set()

    # ---------- HTTP 线程侧 ----------

    def pick_files(self, timeout: float = 600) -> list[str] | None:
        """弹"选择文件"对话框；用户取消返回空列表，**超时返回 ``None``**。"""
        return self._request("files", timeout)

    def pick_folder(self, timeout: float = 600) -> str | None:
        """弹"选择文件夹"对话框；超时/取消返回 ``None``。"""
        return self._request("folder", timeout) or None

    def _request(self, kind: str, timeout: float):
        reply: "queue.Queue" = queue.Queue(maxsize=1)
        self._requests.put((kind, reply))
        try:
            return reply.get(timeout=timeout)
        except queue.Empty:
            return None
