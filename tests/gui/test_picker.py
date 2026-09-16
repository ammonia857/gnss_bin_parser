"""Task 6：原生对话框桥的请求/响应管线测试（注入假对话框，不启动真实 UI）。"""
import threading
import unittest

from gui.native_picker import NativePicker


class PickerTest(unittest.TestCase):
    def test_pick_files_roundtrip_with_injected_dialog(self):
        picker = NativePicker(dialog_files=lambda: [r"C:\a.bin", r"C:\b.bin"])
        threading.Thread(target=picker.run_forever, daemon=True).start()
        try:
            self.assertEqual(picker.pick_files(timeout=5), [r"C:\a.bin", r"C:\b.bin"])
        finally:
            picker.stop()

    def test_pick_folder_roundtrip(self):
        picker = NativePicker(dialog_folder=lambda: r"C:\data")
        threading.Thread(target=picker.run_forever, daemon=True).start()
        try:
            self.assertEqual(picker.pick_folder(timeout=5), r"C:\data")
        finally:
            picker.stop()

    def test_cancel_or_empty_selection_returns_empty(self):
        # 两个对话框都注入假实现：测试期间绝不弹出真实系统窗口
        picker = NativePicker(dialog_files=lambda: [], dialog_folder=lambda: None)
        threading.Thread(target=picker.run_forever, daemon=True).start()
        try:
            self.assertEqual(picker.pick_files(timeout=5), [])
            self.assertIsNone(picker.pick_folder(timeout=5))
        finally:
            picker.stop()

    def test_timeout_returns_none_when_no_worker(self):
        picker = NativePicker(dialog_files=lambda: [r"C:\a.bin"])
        self.assertIsNone(picker.pick_files(timeout=0.2))   # 未 run_forever → 超时


if __name__ == "__main__":
    unittest.main()
