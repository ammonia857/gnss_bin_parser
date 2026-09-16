import json, tempfile, unittest
from pathlib import Path
from gui.state import State, DEFAULTS

class StateTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.path = Path(self.td.name) / "state.json"

    def tearDown(self):
        self.td.cleanup()

    def test_defaults_when_missing(self):
        s = State(self.path); s.load()
        self.assertEqual(s.settings["autoStart"], True)
        self.assertEqual(s.settings["port"], 8765)
        self.assertEqual(s.jobs, [])

    def test_roundtrip(self):
        s = State(self.path); s.load()
        s.set_settings({"outputDir": r"D:\out", "autoStart": False})
        s.set_jobs([{"id": "j1", "status": "queued"}])
        s.save()
        s2 = State(self.path); s2.load()
        self.assertEqual(s2.settings["outputDir"], r"D:\out")
        self.assertEqual(s2.settings["autoStart"], False)
        self.assertEqual(s2.jobs[0]["id"], "j1")

    def test_corrupted_file_falls_back_to_defaults(self):
        self.path.write_text("{ not json", encoding="utf-8")
        s = State(self.path); s.load()
        self.assertEqual(s.settings["port"], DEFAULTS["port"])
        self.assertEqual(s.jobs, [])

    def test_atomic_write_leaves_no_temp_file(self):
        s = State(self.path); s.load(); s.set_settings({"port": 8770}); s.save()
        leftovers = [p.name for p in Path(self.td.name).iterdir() if p.name != "state.json"]
        self.assertEqual(leftovers, [])
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8"))["settings"]["port"], 8770)

    def test_unknown_settings_keys_are_kept_out(self):
        s = State(self.path); s.load()
        s.set_settings({"bogus": 1})
        self.assertNotIn("bogus", s.settings)
