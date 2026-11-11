import hashlib
import os
import subprocess
import sys
import time


def run(executable, mode, enabled, verify=False, negative=False):
    env = os.environ.copy()
    for name in ("APS5_DISPATCH_SPACE_REUSE", "APS5_VERIFY_DISPATCH_SPACE_REUSE"):
        env.pop(name, None)
    if enabled:
        env["APS5_DISPATCH_SPACE_REUSE"] = "1"
    if verify:
        env["APS5_VERIFY_DISPATCH_SPACE_REUSE"] = "1"
    start = time.monotonic()
    process = subprocess.run([executable, mode], env=env, capture_output=True, text=True, timeout=25)
    elapsed = time.monotonic() - start
    text = process.stdout + process.stderr
    if process.returncode == 77:
        print(text)
        sys.exit(77)
    if "VUID-" in text or "was not found but was requested by env var VK_INSTANCE_LAYERS" in text:
        raise AssertionError(text)
    if negative:
        assert process.returncode != 0 and "required two actual feature hits" in text, text
        print("negative activation: require-hit failed with the feature environment removed")
        return {}
    if process.returncode != 0:
        raise AssertionError(text)
    rows = {}
    for line in process.stdout.splitlines():
        if line.startswith("BYTES "):
            _, label, encoded = line.split()
            assert label not in rows, label
            rows[label] = bytes.fromhex(encoded)
        elif line.startswith("COUNTS "):
            print(mode, line)
    assert rows, text
    assert all(len(value) == 65536 for value in rows.values()), "missing guest output or guards"
    print(f"{mode}: {elapsed:.3f}s")
    return rows


executable, mode = sys.argv[1:]
if mode == "negative":
    run(executable, "--require-hit", False, negative=True)
elif mode in ("compare", "verify"):
    old = run(executable, "--old", False)
    new = run(executable, "--verify" if mode == "verify" else "--require-hit", True, mode == "verify")
    assert old.keys() == new.keys(), "different output rounds"
    for label in old:
        assert old[label] == new[label], f"OLD/new guest output or guards differ in {label}"
        print(f"OLD=={'VERIFY' if mode == 'verify' else 'ON'} {label}: {len(old[label])} bytes sha256={hashlib.sha256(old[label]).hexdigest()}")
    assert old["repeat-0"] == old["repeat-1"] == old["repeat-2"], "identical inputs gave different bytes"
    assert old["repeat-2"] != old["changed-input"], "changed BDA input was not read"
else:
    run(executable, "--" + mode, True)
