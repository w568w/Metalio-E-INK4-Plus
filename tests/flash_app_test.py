"""Host tests for the flasher's startup cutoff and panic handling."""
import argparse
import importlib.util
from pathlib import Path
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location("flash_app", Path(__file__).parents[1] / "tools/firmware/flash_app.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)

class FakePort:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.closed = False
        self.dtr = True
    def read(self, _):
        return next(self.chunks)
    def close(self):
        self.closed = True

class FlashAppTests(unittest.TestCase):
    def test_returns_as_soon_as_selected_milestone_arrives(self):
        with tempfile.TemporaryDirectory() as temporary:
            port = FakePort([b"HomeScreen: home page 1/2", b"SD ok at /sdcard"])
            args = argparse.Namespace(ready="storage", boot_timeout=90, observe=0)
            timings = {}
            flash.observe(port, "unused", args, Path(temporary), timings, time.monotonic())
            self.assertIn("boot_home", timings)
            self.assertIn("boot_storage", timings)
            self.assertTrue(port.closed)
            self.assertTrue((Path(temporary) / "flash-timings.json").exists())
    def test_panic_is_not_reported_as_startup_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            port = FakePort([b"Guru Meditation Error"])
            args = argparse.Namespace(ready="home", boot_timeout=90, observe=0)
            with self.assertRaisesRegex(RuntimeError, "启动异常"):
                flash.observe(port, "unused", args, Path(temporary), {}, time.monotonic())
            self.assertTrue(port.closed)
            self.assertFalse((Path(temporary) / "flash-timings.json").exists())

if __name__ == "__main__":
    unittest.main()
