import copy
import json
import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tools"))

from archb_gate_config import GateError
from archb_gate_stats import diagnostics
from archb_recorder_gate import measure, verdict


def overhead(directory):
    blocks, lines, events, records = [], [], [], []
    for index in range(96):
        on = (index % 2) ^ ((index // 2) % 2)
        start = 1_000_000_000 + index * 15_000_000_000
        blocks.append({"role": "candidate" if on else "control", "start_seq": index + 1,
                       "end_seq": index + 2, "values": {"RECORDER": str(on)}})
        lines.append(f"[archb] toggle seq={index + 1} t_ns={start} RECORDER={on}")
        for offset in range(150):
            events.append({"ev": "present", "n": len(events) + 1, "t_ns": start + offset * 100_000_000, "attempts": len(events) + 1, "presented": len(events) + 1, "acquire_dropped": 0, "out_of_date": 0})
        records.append({"archb": {"recorder_toggled": False, "recorder_enabled": bool(on),
                                  "toggle_seq": index + 1, "start_ns": start + 5_000_000_000,
                                  "end_ns": start + 10_000_000_000, "frames": 50,
                                  "counters": {"finisher_handoffs": 100, "finisher_inline": 10,
                                               "draw_pipeline_commits": 110}}})
    lines.append(f"[archb] toggle seq=97 t_ns={1_000_000_000 + 96 * 15_000_000_000} RECORDER=0")
    (directory / "blocks.json").write_text(json.dumps(blocks))
    (directory / "stderr.log").write_text("\n".join(lines) + "\n")
    (directory / "videoout.jsonl").write_text("".join(json.dumps(event) + "\n" for event in events))
    path = directory / "aps5-summary.jsonl"
    def write():
        path.write_text("".join(json.dumps(record) + "\n" for record in records))
    write()
    result = measure(directory, 3)
    assert result["verdict"] == "pass" and result["stage"] == 48 and result["scheduling"]["pass"]
    for record in records:
        if record["archb"]["recorder_enabled"]:
            record["archb"]["counters"]["finisher_handoffs"] = 101
    write()
    result = measure(directory, 3)
    assert result["verdict"] == "fail" and not result["scheduling"]["pass"]
    del records[0]["archb"]["counters"]["finisher_handoffs"]
    write()
    try:
        measure(directory, 3)
        raise AssertionError("missing scheduling counters accepted")
    except GateError as error:
        assert error.code == 22


def run():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        row = {"kind": "draw", "code": "abcdef", "calls": 10, "prepare_ms": 60,
               "record_ms": 20, "proof_ms": 10, "bind_ms": 10}
        record = {"frames": 10, "presented": 10, "start_ns": 4_000_000_000, "end_ns": 9_000_000_000,
                  "toggle_seq": 1, "recorder_enabled": True, "recorder_toggled": False,
                  "phases_ms": {"queue worker 0": {"prepare": 100}}, "counters": {"required_hit": 7},
                  "shaders": {"top": [row], "total": row, "other": {}},
                  "accounted_cpu_ms_per_frame": 10, "unaccounted_ms_per_frame": 490}
        blocks = [{"role": "candidate", "start_seq": 1, "end_seq": 2, "values": {"RECORDER": "1", "GATE_NOOP": "1"}},
                  {"role": "control", "start_seq": 2, "end_seq": 3, "values": {"RECORDER": "1", "GATE_NOOP": "0"}}]
        (directory / "blocks.json").write_text(json.dumps(blocks))
        (directory / "recorder-config.json").write_text(json.dumps({"requiredCounters": ["required_hit"],
                                                                   "switches": blocks[0]["values"], "settle_seconds": 3}))
        (directory / "hold.json").write_text(json.dumps({"start_ns": 1_000_000_000, "end_ns": 30_000_000_000}))
        (directory / "stderr.log").write_text("[archb] toggle seq=1 t_ns=1000000000 RECORDER=1 GATE_NOOP=1\n"
                                              "[archb] toggle seq=2 t_ns=16000000000 RECORDER=1 GATE_NOOP=0\n"
                                              "[archb] toggle seq=3 t_ns=31000000000 RECORDER=1 GATE_NOOP=1\n")
        tagged = copy.deepcopy(record)
        tagged.update(recorder_toggled=True, counters={"required_hit": 1000})
        control = copy.deepcopy(record)
        control.update(toggle_seq=2, start_ns=20_000_000_000, end_ns=25_000_000_000, counters={"required_hit": 1000})
        warmup = copy.deepcopy(record)
        warmup.update(start_ns=0, end_ns=1_000_000_000, counters={"required_hit": 1000})
        settling = copy.deepcopy(record)
        settling.update(start_ns=2_000_000_000, end_ns=3_000_000_000, counters={"required_hit": 1000})
        path = directory / "aps5-summary.jsonl"
        def write(items):
            path.write_text("".join(json.dumps({"archb": item}) + "\n" for item in items))
        write([tagged, control, warmup, settling, record])
        result = diagnostics(directory)
        assert result["source"] == "recorder" and result["frames"] == 10
        assert result["counters"]["required_hit"] == 7
        assert result["shader_exclusive_cpu"]["draw:abcdef"] == 10
        assert result["accounted_cpu_ms_per_frame"] == 10
        record["counters"]["required_hit"] = 0
        write([tagged, control, warmup, settling, record])
        try:
            diagnostics(directory)
            raise AssertionError("noncandidate counters satisfied the requirement")
        except GateError as error:
            assert error.code == 22
        write([tagged, control])
        try:
            diagnostics(directory)
            raise AssertionError("empty candidate intervals accepted")
        except GateError as error:
            assert error.code == 24
    def pairs(count, mean, spread=0):
        return [{"control": {"ms": 100, "p95": 100},
                 "candidate": {"ms": 100 * math.exp(mean + (spread if i % 2 else -spread)), "p95": 100}}
                for i in range(count)]
    assert verdict(pairs(12, 0.005))["verdict"] == "pass"
    assert verdict(pairs(12, 0.03))["verdict"] == "fail"
    assert verdict(pairs(12, 0.008, 0.025))["verdict"] == "continue"
    fallback = verdict(pairs(48, 0.008, 0.025))
    assert fallback["verdict"] == "pass" and fallback["accepted_at_two_percent"]
    assert verdict(pairs(48, 0.008, 0.1))["verdict"] == "inconclusive"
    with tempfile.TemporaryDirectory() as temporary:
        overhead(Path(temporary))


if __name__ == "__main__":
    run()
