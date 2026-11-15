import hashlib
import os
import subprocess
import sys


def run(executable, mode, enabled, verify=False, negative=False, table_cache=False):
    env = os.environ.copy()
    for name in ("APS5_ARCHB_DISPATCH_ADDRESS_REUSE", "APS5_VERIFY_DISPATCH_ADDRESS_REUSE", "APS5_DISPATCH_SPACE_REUSE", "APS5_VERIFY_DISPATCH_SPACE_REUSE", "APS5_NO_BDA_TABLE_CACHE", "APS5_NO_RESOURCE_CACHE", "APS5_NO_POOL_CACHE", "APS5_NO_LAYOUT_CACHE"):
        env.pop(name, None)
    env["APS5_ARCHB_DISPATCH_ADDRESS_REUSE"] = "1" if enabled else "0"
    env["APS5_RELEASE_ON_UNLOCK"] = "1"
    env["APS5_NO_OPPORTUNISTIC_REAP"] = "1"
    if not table_cache:
        env["APS5_NO_BDA_TABLE_CACHE"] = "1"
    if mode == "--no-pool":
        env["APS5_NO_POOL_CACHE"] = "1"
    if verify:
        env["APS5_VERIFY_DISPATCH_ADDRESS_REUSE"] = "1"
    process = subprocess.run([executable, mode], env=env, capture_output=True, text=True, timeout=25)
    text = process.stdout + process.stderr
    if process.returncode == 77:
        print(text)
        sys.exit(77)
    assert "VUID-" not in text and "was not found but was requested by env var VK_INSTANCE_LAYERS" not in text, text
    if negative:
        assert process.returncode != 0 and "required retired-set reuse" in text, text
        print("negative activation: real part reuse was required and failed with the feature disabled")
        return {}
    assert process.returncode == 0, text
    rows = {}
    for line in process.stdout.splitlines():
        if line.startswith("BYTES "):
            _, label, encoded = line.split()
            assert label not in rows, label
            rows[label] = bytes.fromhex(encoded)
        elif line.startswith("COUNTS "):
            print(mode, line)
    assert rows and all(len(value) == 65536 for value in rows.values()), text
    return rows


executable, mode = sys.argv[1:]
if mode == "negative":
    run(executable, "--on", False, negative=True)
elif mode in ("compare", "verify"):
    old = run(executable, "--old", False, table_cache=mode == "cache")
    new = run(executable, "--verify" if mode == "verify" else "--on", True, mode == "verify", table_cache=mode == "cache")
    assert old.keys() == new.keys(), "different output rounds"
    for label in old:
        assert old[label] == new[label], "OLD/new output or guards differ: " + label
        print(f"OLD==NEW {label}: {len(old[label])} bytes sha256={hashlib.sha256(old[label]).hexdigest()}")
    assert old["changed-key-0"] != old["changed-key-1"], "different descriptor and data words did not alter output"
else:
    run(executable, "--" + mode, True, verify=True, table_cache=mode == "cache")
