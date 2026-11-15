import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(executable, mode, enabled, verify, negative=False):
    env = os.environ.copy()
    for name in ("APS5_ARCHB_DISPATCH_UPLOAD_REUSE", "APS5_VERIFY_DISPATCH_UPLOAD_REUSE", "APS5_DISPATCH_SPACE_REUSE", "APS5_NO_ADDRESS_SPACE_CACHE", "APS5_ARCHB_CONTROL_FILE"):
        env.pop(name, None)
    env["APS5_ARCHB_DISPATCH_UPLOAD_REUSE"] = "1" if enabled else "0"
    env["APS5_RELEASE_ON_UNLOCK"] = "1"
    env["APS5_NO_OPPORTUNISTIC_REAP"] = "1"
    env["APS5_ARCHB_GATE_RUN"] = "1"
    if verify:
        env["APS5_VERIFY_DISPATCH_UPLOAD_REUSE"] = "1"
    with tempfile.TemporaryDirectory(prefix="dispatch-upload-reuse-") as temporary:
        env["APS5_ARCHB_CONTROL_FILE"] = str(Path(temporary) / "switches.txt")
        process = subprocess.run([executable, "--upload-" + mode], env=env, capture_output=True, text=True, timeout=25)
    text = process.stdout + process.stderr
    if process.returncode == 77:
        print(text)
        sys.exit(77)
    assert "VUID-" not in text and "Validation Error" not in text and "was not found but was requested by env var VK_INSTANCE_LAYERS" not in text, text
    if negative:
        assert process.returncode != 0 and "enabled dispatch did not enter UploadFinish reuse" in text, text
        print("negative activation: the real UploadFinish path required an enabled reuse build")
        return {}
    assert process.returncode == 0, text
    rows = {}
    for line in process.stdout.splitlines():
        if line.startswith("BYTES "):
            _, label, encoded = line.split()
            assert label not in rows, label
            rows[label] = bytes.fromhex(encoded)
        elif line.startswith("UPLOAD "):
            print(mode, line)
    assert len(rows) == 5 and all(len(value) == 65536 for value in rows.values()), text
    return rows


executable, mode = sys.argv[1:]
if mode == "negative":
    run(executable, "on", False, False, True)
elif mode in ("compare", "verify"):
    old = run(executable, "old", False, False)
    new = run(executable, "on", True, mode == "verify")
    assert old.keys() == new.keys()
    for label in old:
        assert old[label] == new[label], "OLD/new output or guards differ: " + label
        print(f"OLD==NEW {label}: {len(old[label])} bytes sha256={hashlib.sha256(old[label]).hexdigest()}")
    assert old["upload-0"] != old["upload-1"], "different data and descriptor words did not change output"
else:
    run(executable, mode, True, True)
