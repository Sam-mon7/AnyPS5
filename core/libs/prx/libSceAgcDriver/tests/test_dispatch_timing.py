import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable, analyzer, directory, mode = sys.argv[1:5]
expect_on = mode == "on" or "--expect-on" in sys.argv[5:]
Path(directory).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=f"dispatch-{mode}-", dir=directory) as output:
    environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_FRAMES="0:1", APS5_TRACE_DIR=output, APS5_TEST_DISPATCH_TIMING="1", APS5_ARCHB_DISPATCH_TIMING="1" if mode == "on" else "0")
    result = subprocess.run([executable], env=environment, capture_output=True, text=True, timeout=25)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    if result.returncode == 77:
        sys.exit(77)
    assert result.returncode == 0, result.returncode
    assert "dispatch data hit words tests passed" in result.stdout
    assert "VUID-" not in result.stderr and "Validation Error" not in result.stderr
    assert "was not found but was requested by env var VK_INSTANCE_LAYERS" not in result.stderr
    traces = list(Path(output).glob("aps5-critical-path-*.jsonl"))
    assert len(traces) == 1, traces
    spec = importlib.util.spec_from_file_location("dispatch_timing", analyzer)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with traces[0].open(encoding="utf-8") as source:
        trace = module.read_trace(source)
    report = module.summarize(trace)
    assert report["complete_calls"] >= 5, report
    assert report["incomplete_calls"] == 0, report
    assert trace["window"]["closed"], trace["window"]
    events = list(trace["events"].values())
    details = [e for e in events if e["kind"] == "phase_begin" and e["name"].startswith("Dispatch.Detail.")]
    ends = {e["value"] for e in events if e["kind"] == "phase_end"}
    assert all(e["id"] in ends for e in details), "unclosed diagnostic scope"
    points = [e for e in events if e["kind"] == "point" and e["name"].startswith("dispatch.")]
    if expect_on:
        assert details and points, "diagnostic activation assertion failed"
        names = {e["name"] for e in details}
        assert {"Dispatch.Detail.Capture", "Dispatch.Detail.Validate", "Dispatch.Detail.HitMaterialize", "Dispatch.Detail.RecordMarks"} <= names, names
        names = {e["name"] for e in points}
        assert {"dispatch.site", "dispatch.key", "dispatch.cache.hit", "dispatch.cache.miss", "dispatch.insert.stable", "dispatch.recipe.build_failure"} <= names, names
        registers = [e for e in points if e["name"] == "dispatch.register"]
        assert any(e["object"] == 0x228 for e in registers)
        assert any(e["value"] == 1 << 32 for e in registers), "absent-register sentinel was lost"
    else:
        assert not details and not points, "disabled diagnostic emitted payload"
    damaged = dict(trace, loss=True)
    try:
        module.summarize(damaged)
    except ValueError:
        pass
    else:
        raise AssertionError("loss was accepted")
    print(json.dumps(dict(mode=mode, calls=report["complete_calls"], scopes=len(details), points=len(points), incomplete=report["incomplete_calls"])))
