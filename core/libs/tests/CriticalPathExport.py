import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable, walker, directory = sys.argv[1:4]
Path(directory).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="export-", dir=directory) as output:
    environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_DIR=output)
    environment["APS5_TRACE_FRAMES"] = "0:1000000"
    subprocess.run([executable, "export"], env=environment, check=True, timeout=30)
    traces = list(Path(output).glob("aps5-critical-path-*.jsonl"))
    assert len(traces) == 1, traces
    result = subprocess.run([sys.executable, walker, str(traces[0]), "--skip-frames", "0", "--json"], capture_output=True, text=True, check=True, timeout=30)
    report = json.loads(result.stdout)
    assert report["selected_frames"] == 1, report
    assert not any("unmatched phase" in item or "emitted count" in item for item in report["warnings"]), report
    print("Recorder output accepted by critical_path.py")
with tempfile.TemporaryDirectory(prefix="autoclose-", dir=directory) as output:
    environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_DIR=output, APS5_TRACE_FRAMES="1:2")
    subprocess.run([executable, "autoclose"], env=environment, check=True, timeout=30)
    traces = list(Path(output).glob("aps5-critical-path-*.jsonl"))
    assert len(traces) == 1, traces
    rows = [json.loads(line) for line in traces[0].read_text().splitlines()]
    assert rows[-1] == {"type": "footer", "complete": True}, rows[-1]
    assert sum(isinstance(row, list) and row[3] == "present" for row in rows) == 3
    result = subprocess.run([sys.executable, walker, str(traces[0]), "--skip-frames", "0", "--frames", "2", "--json"], capture_output=True, text=True, check=True, timeout=30)
    report = json.loads(result.stdout)
    assert report["selected_frames"] == 2, report
    print("Automatic capture footer survived exit without atexit callbacks")
if "--linked" in sys.argv[4:]:
    with tempfile.TemporaryDirectory(prefix="linked-return-", dir=directory) as output:
        environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_DIR=output)
        environment["APS5_TRACE_FRAMES"] = "0:1000000"
        subprocess.run([executable, "natural-return"], env=environment, check=True, timeout=30)
        traces = list(Path(output).glob("aps5-critical-path-*.jsonl"))
        assert len(traces) == 1, traces
        rows = [json.loads(line) for line in traces[0].read_text().splitlines()]
        assert rows[-1] == {"type": "footer", "complete": True}, rows[-1]
        result = subprocess.run([sys.executable, walker, str(traces[0]), "--skip-frames", "0", "--json"], capture_output=True, text=True, check=True, timeout=30)
        report = json.loads(result.stdout)
        assert report["selected_frames"] == 1 and not report["truncated"], report
        print("Linked libc natural return completed trace without explicit test shutdown")
with tempfile.TemporaryDirectory(prefix="default-window-", dir=directory) as output:
    environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_DIR=output)
    environment.pop("APS5_TRACE_FRAMES", None)
    subprocess.run([executable, "default-window"], env=environment, check=True, timeout=30)
    traces = list(Path(output).glob("aps5-critical-path-*.jsonl"))
    assert len(traces) == 1, traces
    rows = [json.loads(line) for line in traces[0].read_text().splitlines()]
    events = [row for row in rows if isinstance(row, list)]
    assert len([row for row in events if row[3] == "present"]) == 61
    assert not any(row[3] == "phase_mark" or "never-record" in row[4] for row in events)
    names = {row["name"] for row in rows if isinstance(row, dict) and row.get("type") == "thread_metadata"}
    assert names == {"startup-main", "window-worker"}, names
    result = subprocess.run([sys.executable, walker, str(traces[0]), "--json"], capture_output=True, text=True, check=True, timeout=30)
    report = json.loads(result.stdout)
    assert report["selected_frames"] == 60 and not report["truncated"], report
    print("Default window retained names, omitted startup/marks, and analyzed exactly 60 frames")
