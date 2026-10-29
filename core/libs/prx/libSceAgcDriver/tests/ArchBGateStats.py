import json
import math
from pathlib import Path
import statistics
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tools"))

from archb_gate_config import GateError
from archb_gate_stats import T_95, alternating_blocks, alternating_verdict, bootstrap, diagnostics, final_verdict, leg_summary, pair_report, sequential, t_critical, tail_check


def fails(call, code=24):
    try:
        call()
    except GateError as error:
        assert error.code == code, (error.code, code)
    else:
        raise AssertionError("expected gate failure")


def event(frame, timestamp, presented=None, acquire_dropped=0, out_of_date=0):
    return {"ev": "present", "n": frame, "t_ns": timestamp, "attempts": frame,
            "presented": frame if presented is None else presented, "acquire_dropped": acquire_dropped, "out_of_date": out_of_date}


def write_events(path, events):
    path.write_text("\n".join(map(json.dumps, events)) + "\n", encoding="utf-8")


def fixture(directory, intervals, step=1):
    timestamp = 1000000000
    events = [event(1, timestamp)]
    for index, milliseconds in enumerate(intervals, 1):
        timestamp += round(milliseconds * 1e6)
        events.append(event(1 + index * step, timestamp))
    path = Path(directory) / "videoout.jsonl"
    write_events(path, events)
    return path, events


def summary(directory, intervals, step=1):
    path, events = fixture(directory, intervals, step)
    return leg_summary(path, events[0]["t_ns"], events[-1]["t_ns"], sum(intervals) / 1000)


def pairs(deltas):
    counts = {"attempts": 100, "presented": 100, "acquire_dropped": 0, "out_of_date": 0}
    return [{"control": {**counts, "ms": 100, "p95": 110}, "candidate": {**counts, "ms": 100 * math.exp(delta), "p95": 110}} for delta in deltas]


def frame_deltas_compute_ms(directory):
    result = summary(directory, [100, 200, 100, 100, 200])
    assert result["ms"] == 140
    assert math.isclose(result["fps"], 1000 / 140)
    assert result["p95"] == 200 and result["p50"] == 100 and result["longest_interval_ms"] == 200
    path, events = fixture(directory, [100] * 20)
    assert leg_summary(path, events[3]["t_ns"], events[15]["t_ns"], 1.2)["frames"] == 13
    fails(lambda: leg_summary(path, 0, events[-1]["t_ns"], 10))
    (Path(directory) / "stderr.log").write_text(
        "[draw-shaders] 10 s, 2 draw packets; slowest:\n  vertex/main=0x123: 2 packets (2 drawn, 0 captures), 42.0 ms (commit 3), 1 us each\n"
        "[dispatch-shaders] 10 s, 1 dispatch packet; slowest:\n  0x456 real: 1 packets (real 1), 21.0 ms, 1 us each\n"
        "[archb] queue0_packet_ms=210 worker_ms=42\n", encoding="utf-8")
    (Path(directory) / "aps5-summary.jsonl").write_text(json.dumps({"interval_start_s": 0, "interval_end_s": 2, "presents": 21,
        "top_phases_ms_per_frame": {"Driver.Packet": 10}}) + "\n", encoding="utf-8")
    report = diagnostics(directory)
    assert report["shader_exclusive_cpu"]["draw:vertex/main=0x123"] == 2
    assert report["shader_exclusive_cpu"]["dispatch:0x456 real"] == 1
    assert report["trace_scopes"]["Driver.Packet"][0]["total_ms"] == 210
    assert report["accounted_cpu_ms_per_frame"] == 12
    stderr = (Path(directory) / "stderr.log").read_bytes()
    prefix = b"[archb] queue0_packet_ms=99999 worker_ms=99999\n"
    (Path(directory) / "stderr.log").write_bytes(prefix + stderr + prefix)
    (Path(directory) / "hold.json").write_text(json.dumps({"start_ns": events[5]["t_ns"], "end_ns": events[15]["t_ns"],
        "stderr_start": len(prefix), "stderr_end": len(prefix) + len(stderr)}), encoding="utf-8")
    report = diagnostics(directory)
    assert report["frames"] == 11 and math.isclose(report["accounted_cpu_ms_per_frame"], 252 / 11)
    assert report["trace_scopes"] == {} and report["trace_summaries"] == []


def gap_or_duplicate_fails(directory):
    for field, delta in (("n", 1), ("n", -1), ("t_ns", -100000000), ("t_ns", -200000000)):
        path, events = fixture(directory, [100] * 10)
        events[5][field] += delta
        path.write_text("\n".join(json.dumps(event) for event in events), encoding="utf-8")
        fails(lambda: leg_summary(path, 0, 3000000000, 1))


def aa_noise_sets_margin(directory):
    for deltas in ([0] * 6, [-0.03, 0.03, -0.03, 0.03, 0, 0]):
        result = bootstrap(pairs(deltas))
        assert result["code"] == 0
        assert math.isclose(result["s_AA"], statistics.stdev(deltas), abs_tol=1e-14)
        assert result["m"] == 0.02


def noise_above_limit_fails(directory):
    result = bootstrap(pairs([-0.1, 0.1] * 3))
    assert result["code"] == 40 and result["s_AA"] > 0.033


def sequential_pass_at_four(directory):
    result = sequential(pairs([0.001, -0.001, 0.002, -0.002]), 0.02)
    assert result["code"] == 0 and result["verdict"] == "pass" and result["stage"] == 4
    assert result["UB"] <= math.log1p(0.02)
    assert result["speedup_90_interval"][0] < 1 < result["speedup_90_interval"][1]


def sequential_continue_then_fail(directory):
    first = pairs([0.06, -0.06, 0.06, -0.06])
    result = sequential(first, 0.02)
    assert result["verdict"] == "continue"
    final = sequential(first + pairs([0.3] * 4), 0.02)
    assert final["code"] == 1 and final["verdict"] == "fail" and final["stage"] == 8
    ambiguous = sequential(pairs([0.06, -0.06] * 4), 0.02)
    assert ambiguous["code"] == 3 and ambiguous["verdict"] == "inconclusive" and ambiguous["LB"] <= math.log1p(0.02) < ambiguous["UB"]


def clear_regression_fails_early(directory):
    result = sequential(pairs([0.1, 0.101, 0.099, 0.1]), 0.02)
    assert result["code"] == 1 and result["stage"] == 4 and result["LB"] > math.log1p(0.02)


def p95_median_rule(directory):
    values = pairs([0] * 4)
    for pair, ratio in zip(values, [1, 1.02, 1.04, 2]):
        pair["candidate"]["p95"] = pair["control"]["p95"] * ratio
    assert tail_check(values)["tail_code"] == 0
    for pair, ratio in zip(values, [1.04, 1.06, 1.08, 1.10]):
        pair["candidate"]["p95"] = pair["control"]["p95"] * ratio
    assert tail_check(values)["tail_code"] == 2


def final_mode_requires_every_leg(directory):
    good = summary(directory, [60] * 100)
    slow = summary(directory, [70] * 100)
    assert final_verdict([good] * 3)["code"] == 0
    assert final_verdict([{**good, "ms": 1000 / 15}] * 3)["code"] == 0
    assert final_verdict([{**good, "ms": 66.68}] * 3)["code"] == 1
    assert final_verdict([{**good, "attempts": 100, "presented": 98}] * 3)["code"] == 27
    for index in range(3):
        legs = [good] * 3
        legs[index] = slow
        assert final_verdict(legs)["code"] == 1


def old_30_frame_telemetry_rejected(directory):
    fails(lambda: summary(directory, [4200] * 30, 30))


def alternating_fixture(directory, count=24):
    blocks, lines, events = [], [], []
    frame = 1
    for index in range(count):
        role = ("control", "candidate")[(index % 2) ^ ((index // 2) % 2)]
        start = 1000000000 + index * 15000000000
        values = {"GATE_NOOP": "1" if role == "candidate" else "0", "GATE_DELAY_US": "0"}
        blocks.append({"role": role, "start_seq": index + 1, "end_seq": index + 2, "values": values})
        lines.append(f"[archb] toggle seq={index + 1} t_ns={start} GATE_NOOP={values['GATE_NOOP']} GATE_DELAY_US=0")
        timestamp = start
        while timestamp < start + 15000000000:
            events.append(event(frame, timestamp))
            milliseconds = 500 if timestamp < start + 3000000000 else 100 if role == "control" else 200
            timestamp += milliseconds * 1000000
            frame += 1
    lines.append(f"[archb] toggle seq={count + 1} t_ns={1000000000 + count * 15000000000} GATE_NOOP=0 GATE_DELAY_US=0")
    telemetry = Path(directory) / "videoout.jsonl"
    telemetry.write_text("\n".join(map(json.dumps, events)) + "\n", encoding="utf-8")
    (Path(directory) / "stderr.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (Path(directory) / "blocks.json").write_text(json.dumps(blocks), encoding="utf-8")
    return telemetry, "\n".join(lines), blocks, events


def alternating_block_pairing(directory):
    path, text, blocks, events = alternating_fixture(directory)
    result = alternating_blocks(path, text, blocks, 3)
    assert len(result) == 12
    assert all(pair["control"]["ms"] == 100 and pair["candidate"]["ms"] == 200 for pair in result)
    assert result[1]["candidate"]["toggle_ns"] < result[1]["control"]["toggle_ns"]
    assert all(math.isclose(delta, math.log(2)) for delta in sequential(result, 0.02, True)["d_i"])
    verdict = alternating_verdict(directory, 3, {"alternating_m": 0.02, "mean_ms": 150})
    assert verdict["code"] == 1 and verdict["stage"] == 12
    fails(lambda: alternating_blocks(path, text.replace("GATE_NOOP=1", "GATE_NOOP=0"), blocks, 3), 21)
    fails(lambda: alternating_blocks(path, text + "\n[archb] toggle refused seq=25 unknown FOO", blocks, 3), 21)
    fails(lambda: alternating_blocks(path, text.rsplit("\n", 1)[0], blocks, 3))
    blocks[1]["role"] = blocks[0]["role"]
    fails(lambda: alternating_blocks(path, text, blocks, 3))
    blocks[1]["role"] = "candidate"
    events[3]["n"] += 1
    path.write_text("\n".join(map(json.dumps, events)) + "\n", encoding="utf-8")
    fails(lambda: alternating_blocks(path, text, blocks, 3))


def alternating_settle_discard(directory):
    path, text, blocks, events = alternating_fixture(directory)
    settled = alternating_blocks(path, text, blocks, 3)
    untrimmed = alternating_blocks(path, text, blocks, 0)
    assert settled[0]["control"]["ms"] == 100 and settled[0]["control"]["p95"] == 100
    assert untrimmed[0]["control"]["ms"] > 100 and untrimmed[0]["control"]["p99"] > 100
    for pair in settled:
        for block in pair.values():
            assert block["start_ns"] == block["toggle_ns"] + 3000000000
    fails(lambda: alternating_blocks(path, text, blocks, 15))
    fails(lambda: alternating_blocks(path, text, blocks, -1))


def alternating_sequential_12_24(directory):
    assert sequential(pairs([0.001, -0.001] * 6), 0.02, True)["verdict"] == "pass"
    assert sequential(pairs([0.05] * 12), 0.02, True)["code"] == 1
    ambiguous = pairs([0.10, -0.10] * 6)
    assert sequential(ambiguous, 0.02, True)["verdict"] == "continue"
    result = sequential(ambiguous + ambiguous, 0.02, True)
    assert result["code"] == 0 and result["verdict"] == "continue" and result["stage"] == 24 and result["LB"] < result["threshold"] < result["UB"]
    assert sequential(ambiguous + pairs([-0.05] * 12), 0.02, True)["verdict"] == "pass"
    result = bootstrap(pairs([0.01, -0.01] * 12), True)
    assert result["code"] == 0 and result["alternating_m"] == 0.02 and result["stage"] == 24
    assert bootstrap(pairs([0.05, -0.05] * 12), True)["code"] == 40
    fails(lambda: sequential(pairs([0] * 4), 0.02, True))
    fails(lambda: bootstrap(pairs([0] * 12), True))


def alternating_t_table(directory):
    assert len(T_95) == 60 and all(a > b for a, b in zip(T_95, T_95[1:]))
    for df, expected in ((1, 6.313752), (3, 2.353363), (7, 1.894579), (11, 1.795885), (23, 1.713872), (60, 1.670649)):
        assert t_critical(df) == expected
    result = sequential(pairs([0.01, -0.01] * 6), 0.02, True)
    radius = t_critical(11) * statistics.stdev(result["d_i"]) / math.sqrt(12)
    assert math.isclose(result["UB"], statistics.mean(result["d_i"]) + radius)
    fails(lambda: t_critical(0))
    fails(lambda: t_critical(61))


def presented_denominator(directory):
    path = Path(directory) / "videoout.jsonl"
    events = [event(index + 1, 1000000000 + round(index * 1e9 / 60), 1 + index // 2, (index + 1) // 2)
              for index in range(121)]
    write_events(path, events)
    result = leg_summary(path, events[0]["t_ns"], events[-1]["t_ns"], 2)
    assert math.isclose(result["ms"], 1000 / 30) and math.isclose(result["fps"], 30)
    assert math.isclose(result["p95"], 1000 / 30, abs_tol=1e-6)
    assert math.isclose(result["p50"], 1000 / 30, abs_tol=1e-6)
    assert result["attempts"] == 120 and result["presented"] == 60 and result["code"] == 27
    for count in (0, 1):
        write_events(path, [event(index + 1, 1000000000 + index * 10000000, 1 + min(index, count)) for index in range(10)])
        fails(lambda: leg_summary(path, 1000000000, 1090000000, 0.09))
    path, events = fixture(directory, [100] * 10)
    for field, value in (("presented", 0), ("attempts", 0), ("acquire_dropped", -1), ("out_of_date", 1.0), ("presented", 99)):
        changed = [dict(item) for item in events]
        changed[5][field] = value
        write_events(path, changed)
        fails(lambda: leg_summary(path, 1000000000, 2000000000, 1))
    del events[5]["presented"]
    write_events(path, events)
    fails(lambda: leg_summary(path, 1000000000, 2000000000, 1))


def drop_limit(directory):
    path = Path(directory) / "videoout.jsonl"
    for drop_count in (1, 2):
        events = [event(index + 1, 1000000000 + index * 10000000,
                        index + 1 - min(index, drop_count), min(index, drop_count)) for index in range(101)]
        write_events(path, events)
        result = leg_summary(path, 1000000000, 2000000000, 1)
        assert result["code"] == (0 if drop_count == 1 else 27)
        assert result["dropped"] == drop_count and result["drop_rate"] == drop_count / 100
        values = [{"control": {**result, "attempts": 100, "presented": 100, "acquire_dropped": 0}, "candidate": result}] * 4
        decision = sequential(values)
        assert decision["code"] == result["code"]
        assert decision["drops"]["candidate"]["dropped"] == 4 * drop_count
        report = pair_report(values, {"mean_ms": 20})
        assert report["baseline_mean_ms"] == 20 and report["candidate_mean_ms"] == result["ms"]
        assert math.isclose(report["cumulative_speedup"], 20 / result["ms"])
        assert final_verdict([result] * 3)["code"] == result["code"]
    path, text, blocks, events = alternating_fixture(directory)
    dropped = 0
    for item in events:
        if item["n"] % 20 == 0:
            dropped += 1
        item.update(presented=item["n"] - dropped, acquire_dropped=dropped)
    write_events(path, events)
    decision = alternating_verdict(directory, 3, {"mean_ms": 100})
    assert decision["code"] == 27 and decision["verdict"] == "present_drops"
    assert decision["drops"]["control"]["dropped"] > 0 and decision["drops"]["candidate"]["dropped"] > 0
    valid = {"ms": 10, "p95": 10, "attempts": 1000, "presented": 1000, "acquire_dropped": 0, "out_of_date": 0}
    invalid = {**valid, "attempts": 100, "presented": 98}
    values = [{"control": valid, "candidate": valid}] * 3 + [{"control": valid, "candidate": invalid}]
    assert sequential(values)["code"] == 27
    assert bootstrap(values + values[:2])["code"] == 27
    fails(lambda: final_verdict([{"ms": 10}] * 3))
    fails(lambda: sequential([{"control": {"ms": 10}, "candidate": {"ms": 10}}] * 4))


def fixed_margin(directory):
    for alternating, count in ((False, 6), (True, 24)):
        scale = 0.03 / statistics.stdev([-1, 1] * (count // 2))
        result = bootstrap(pairs([-scale, scale] * (count // 2)), alternating)
        assert result["code"] == 0 and math.isclose(result["s_AA"], 0.03)
        assert result["m"] == 0.02
        if alternating:
            assert result["alternating_m"] == 0.02
        noise_limit = 0.01 * math.sqrt(48) / t_critical(47)
        for multiplier, expected in ((0.999, 0), (1.001, 40)):
            scale = noise_limit * multiplier / statistics.stdev([-1, 1] * (count // 2))
            assert bootstrap(pairs([-scale, scale] * (count // 2)), alternating)["code"] == expected
    result = sequential(pairs([0.03] * 4), 0.045)
    assert result["m"] == 0.02 and result["code"] == 1


def inconclusive_48(directory):
    for count, expected in ((12, "continue"), (24, "continue"), (48, "inconclusive")):
        result = sequential(pairs([0.1, -0.1] * (count // 2)), alternating=True)
        assert result["verdict"] == expected and result["code"] == (3 if count == 48 else 0)
    assert sequential(pairs([0.001, -0.001] * 24), alternating=True)["verdict"] == "pass"
    assert sequential(pairs([0.05] * 48), alternating=True)["code"] == 1
    path, text, blocks, events = alternating_fixture(directory, 96)
    result = alternating_blocks(path, text, blocks, 3)
    assert len(result) == 48
    decision = alternating_verdict(directory, 3, {"mean_ms": 400})
    assert decision["stage"] == 48 and decision["baseline_mean_ms"] == 400
    assert decision["candidate_mean_ms"] == 200 and decision["cumulative_speedup"] == 2


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="archb-stats-") as directory:
        globals()[sys.argv[1]](directory)
    print(sys.argv[1] + " passed")
