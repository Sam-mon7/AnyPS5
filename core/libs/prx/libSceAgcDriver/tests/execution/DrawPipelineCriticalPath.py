import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


executable, walker, directory, mode = sys.argv[1:5]
Path(directory).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=mode + "-", dir=directory) as output:
    environment = dict(os.environ, APS5_CRITICAL_PATH="1", APS5_TRACE_DIR=output,
                       APS5_TRACE_FRAMES="0:1000000", APS5_PIPELINED_DRAWS="4",
                       APS5_ARCHB_PIPE_Q0_COMMITTER="1",
                       APS5_HANDOFF_SPIN_US="0", APS5_ARCHB_PIPE_FULL_HYSTERESIS="1" if mode == "hysteresis" else "0")
    result = subprocess.run([executable, mode], env=environment, timeout=25)
    if result.returncode == 77:
        sys.exit(77)
    result.check_returncode()
    paths = list(Path(output).glob("aps5-critical-path-*.jsonl"))
    assert len(paths) == 1, paths
    rows = [json.loads(line) for line in paths[0].read_text().splitlines()]
    assert rows[-1] == {"type": "footer", "complete": True}, rows[-1]
    events = {row[0]: dict(zip(("id", "tid", "ns", "kind", "name", "object", "value", "peer", "name_truncated"), row))
              for row in rows if isinstance(row, list)}
    enqueues = sorted((event for event in events.values() if event["kind"] == "release" and event["name"] == "pipe.enqueue"), key=lambda event: event["ns"])
    commits = sorted((event for event in events.values() if event["name"] == "pipe.commit.begin"), key=lambda event: event["ns"])
    retires = sorted((event for event in events.values() if event["name"] == "pipe.retire"), key=lambda event: event["ns"])
    assert len(enqueues) == len(commits) == len(retires), (enqueues, commits, retires)
    assert [event["peer"] for event in commits] == [event["id"] for event in enqueues]
    assert [event["peer"] for event in retires] == [event["id"] for event in enqueues]
    assert [event["value"] for event in retires] == list(range(len(retires)))
    kinds = {event["object"]: event["value"] for event in events.values() if event["name"] == "pipe.item.kind"}
    assert set(kinds) == {event["id"] for event in enqueues}
    full = [event for event in events.values() if event["kind"] == "wait_begin" and event["name"] == "pipe.full"]
    idle = [event for event in events.values() if event["kind"] == "wait_begin" and event["name"] == "pipe.committer.idle.blocked"]
    assert not any(event["name"] == "pipe.committer.idle" for event in events.values())
    ends = {event["value"]: event for event in events.values() if event["kind"] == "wait_end"}
    if mode in {"full", "hysteresis"}:
        assert len(full) == 1 and len(enqueues) == 5, (full, enqueues)
        target_index, target_kind = (1, 3) if mode == "hysteresis" else (0, 2)
        targets = [event for event in events.values() if event["name"] == "pipe.full.target"]
        assert len(targets) == 1, targets
        target = targets[0]
        assert target["object"] == full[0]["id"] and target["peer"] == enqueues[target_index]["id"]
        assert target["value"] == target_kind == kinds[target["peer"]]
        end = ends[full[0]["id"]]
        assert end["name"] == "item_retired" and end["peer"] == retires[target_index]["id"], end
        assert full[0]["ns"] <= retires[target_index]["ns"] <= end["ns"]
        assert not any(event["name"] == "pipe.full.observed_retire" for event in events.values())
    else:
        assert not full, full
    if mode in {"no-block", "overcommit"}:
        assert len(enqueues) == (5 if mode == "overcommit" else 3)
        assert not any(commits[0]["ns"] <= event["ns"] <= commits[-1]["ns"] for event in idle), idle
    if mode == "idle":
        assert len(enqueues) == 2
        paired = [event for event in idle if event["id"] in ends and ends[event["id"]]["peer"] == enqueues[1]["id"]]
        assert len(paired) == 1 and retires[0]["ns"] <= paired[0]["ns"] <= enqueues[1]["ns"], paired
    if mode == "stop":
        assert len(enqueues) == 1
        stopped = [event for event in idle if event["id"] in ends and not ends[event["id"]]["peer"]]
        assert len(stopped) == 1 and retires[0]["ns"] <= stopped[0]["ns"], stopped
    spec = importlib.util.spec_from_file_location("critical_path", walker)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    trace = module.read_trace(paths[0].read_text().splitlines())
    report = module.analyze(trace, 0, 1)
    assert report["selected_frames"] == 1 and not report["truncated"], report
    assert len(trace["pipe_predecessors"]) == max(0, len(commits) - 1), trace["pipe_predecessors"]
    if full:
        metadata = trace["pipe_waits"][full[0]["id"]]
        assert metadata["exact_wake_source"] and metadata["target_kind_value"] == target_kind, metadata
        assert metadata["target_enqueue_event"] == target["peer"] and metadata["target_retirement_event"] == end["peer"], metadata
    print("Draw pipeline critical path runtime fixture and walker passed:", mode)
