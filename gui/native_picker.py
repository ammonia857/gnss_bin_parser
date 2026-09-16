"""tkinter 原生文件对话框桥。

为什么需要它：浏览器拖拽拿不到本地真实路径（安全限制），而 400MB 级数据不该走上传，
因此由后端弹 **Windows 原生对话框**拿绝对路径。约束是 tkinter 的对话框必须在**主线程**
的事件循环里运行，所以本模块提供：

- :meth:`NativePicker.run_forever`：在主线程常驻，轮询请求队列并执行对话框；
- :meth:`NativePicker.pick_files` / :meth:`pick_folder`：供 HTTP 线程调用（阻塞等待结果，带超时）；
- ``dialog_files`` / ``dialog_folder`` 可注入假实现，便于在无 UI 的测试里验证请求/响应管线。
"""
import contextlib
import os
import queue
import threading
from pathlib import Path


@contextlib.contextmanager
def _silence_c_stderr():
    """临时把文件描述符 2 指向 devnull，吞掉 C 库直接写 stderr 的噪声。

    为什么需要：Tk 在**第一次 update() 时**才解码主题 PNG，libpng 会对这些图刷 39 行
    ``libpng warning: iCCP: known incorrect sRGB profile``。这些警告来自 C 层、直接写 fd 2，
    用 ``contextlib.redirect_stderr`` 拦不住（实测：不加处理时启动即 39 行），而用户看到的
    是一启动就满屏"报错"，会以为程序坏了。只罩住 Tk 初始化那几次 update()，之后照常显示。
    """
    try:
        saved = os.dup(2)
    except OSError:                     # 没有可用的 stderr（极简环境）：直接跳过
        yield
        return
    devnull = os.open(os.devnull, os.O_WRONLY)
    try:
        os.dup2(devnull, 2)
        yield
    finally:
        os.dup2(saved, 2)
        os.close(devnull)
        os.close(saved)


def _real_pick_files() -> list[str]:
    """真实"选择 .bin（可多选）"对话框。"""
    from tkinter import filedialog

    paths = filedialog.askopenfilenames(
        title="选择 BIN 文件",
        filetypes=[("NovAtel BIN", "*.bin"), ("所有文件", "*.*")])
    return [str(Path(p)) for p in paths]


def _real_pick_folder(title: str = "选择包含 BIN 的文件夹") -> str | None:
    """真实"选择文件夹"对话框（标题可定制，便于区分输入/输出目录）。"""
    from tkinter import filedialog

    picked = filedialog.askdirectory(title=title)
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

        with _silence_c_stderr():
            root = tk.Tk()
            root.withdraw()
            # 主题 PNG 在最初几次 update() 里才被解码，噪声集中在这几帧（实测共 39 行）
            for _ in range(10):
                root.update()
        self._root = root
        self._stop.clear()
        try:
            while not self._stop.is_set():
                try:
                    kind, reply, arg = self._requests.get(timeout=0.05)
                except queue.Empty:
                    root.update()          # 维持窗口消息循环（对话框依赖它）
                    continue
                try:
                    if kind == "files":
                        reply.put(self._dialog_files())
                    elif kind == "folder":
                        if arg:
                            try:
                                reply.put(self._dialog_folder(arg))
                            except TypeError:       # 注入的假实现可能不接受标题参数
                                reply.put(self._dialog_folder())
                        else:
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

    def pick_folder(self, timeout: float = 600, title: str | None = None) -> str | None:
        """弹"选择文件夹"对话框；超时/取消返回 ``None``；``title`` 可定制标题。"""
        return self._request("folder", timeout, title) or None

    def _request(self, kind: str, timeout: float, arg=None):
        reply: "queue.Queue" = queue.Queue(maxsize=1)
        self._requests.put((kind, reply, arg))
        try:
            return reply.get(timeout=timeout)
        except queue.Empty:
            return None
