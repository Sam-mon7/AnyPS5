import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tools"))

import archb_gate_nav as nav


def driver_capture_waits(directory):
    capture = nav.DriverCapture(directory / "control.txt", directory / "shots")
    nav.Image.new("RGB", (640, 360), "white").save(capture.shots / "stale.png")
    clock = [0.0]
    def sleep(seconds):
        clock[0] += seconds
    with patch.object(nav.time, "monotonic", side_effect=lambda: clock[0]), patch.object(nav.time, "sleep", side_effect=sleep):
        assert nav.classify(capture.image(), {}) == ("loading", 0.0)
        assert 5 <= clock[0] < 5.1
        with patch.object(nav, "load_refs", return_value={}), patch.object(nav.DriverCapture, "image", return_value=None), patch.object(nav, "press") as press:
            try:
                nav.navigate(0, directory, directory / "pad.txt", directory, timeout=70)
            except nav.GateError as error:
                assert error.code == 13
            else:
                raise AssertionError("missing global timeout")
            press.assert_not_called()
    assert "unknown" not in (directory / "nav.log").read_text(encoding="utf-8")


def driver_capture_classifies(directory):
    capture = nav.DriverCapture(directory / "control.txt", directory / "shots")
    values = nav.np.random.default_rng(1).integers(0, 256, (720, 1280, 3), dtype=nav.np.uint8)
    reference = nav.Image.fromarray(values)
    reference.save(directory / "name_screen.bmp")
    with nav.Image.open(directory / "name_screen.bmp") as image:
        refs = {"name_screen": (nav.feature(image), nav.feature(image, nav.PROMPT_BOX))}
    for index in range(7):
        reference.save(capture.shots / f"old-{index}.png")
    fresh = capture.shots / "fresh.png"
    writes = [0]
    def drop(seconds):
        if writes[0] == 0:
            fresh.write_bytes(b"partial PNG")
        else:
            reference.save(fresh)
        timestamp = time.time_ns() + 1000000
        os.utime(fresh, ns=(timestamp, timestamp))
        writes[0] += 1
    with patch.object(nav.time, "sleep", side_effect=drop):
        image = capture.image()
    assert writes[0] == 2 and image.size == (1280, 720)
    state, score = nav.classify(image, refs)
    assert state == "name_screen" and score >= 0.99
    assert len(list(capture.shots.glob("*.png"))) == 5
    assert nav.classify(nav.Image.new("RGB", image.size), refs)[0] == "loading"


def seq_monotonic(directory):
    capture = nav.DriverCapture(directory / "control.txt", directory / "shots", timeout=0)
    sequences = []
    for index in range(3):
        before = time.time_ns() // 1000
        if index == 1 and os.name == "nt":
            runner = str(Path(nav.__file__).with_name("archb_gate.ps1")).replace("'", "''")
            folder = str(directory).replace("'", "''")
            subprocess.run(["pwsh", "-NoProfile", "-Command",
                            f". '{runner}' -Control unused -Output unused; $null = Write-GateControl '{folder}' @{{ GATE_NOOP = '1' }}"], check=True)
        else:
            assert capture.image() is None
        sequence = int(capture.control.read_text(encoding="ascii").split()[0])
        assert before <= sequence <= time.time_ns() // 1000
        sequences.append(sequence)
        time.sleep(0.001)
    assert sequences == sorted(set(sequences))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="archb-nav-", dir=Path(__file__).resolve().parents[5] / "build") as directory:
        globals()[sys.argv[1]](Path(directory))
    print(sys.argv[1] + " passed")
