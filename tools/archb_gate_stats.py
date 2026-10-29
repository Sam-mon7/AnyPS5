import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

from archb_gate_config import GateError, read_json, write_json


FALSE_PASS_NOTE = "The fixed sequential stages inflate the false-pass rate above 5%; this is accepted."
PRESENT_FIELDS = ("n", "t_ns", "attempts", "presented", "acquire_dropped", "out_of_date")
T_95 = (
    6.313752, 2.919986, 2.353363, 2.131847, 2.015048, 1.943180, 1.894579, 1.859548, 1.833113, 1.812461,
    1.795885, 1.782288, 1.770933, 1.761310, 1.753050, 1.745884, 1.739607, 1.734064, 1.729133, 1.724718,
    1.720743, 1.717144, 1.713872, 1.710882, 1.708141, 1.705618, 1.703288, 1.701131, 1.699127, 1.697261,
    1.695519, 1.693889, 1.692360, 1.690924, 1.689572, 1.688298, 1.687094, 1.685954, 1.684875, 1.683851,
    1.682878, 1.681952, 1.681071, 1.680230, 1.679427, 1.678660, 1.677927, 1.677224, 1.676551, 1.675905,
    1.675285, 1.674689, 1.674116, 1.673565, 1.673034, 1.672522, 1.672029, 1.671553, 1.671093, 1.670649,
)


def t_critical(degrees):
    if type(degrees) is not int or not 1 <= degrees <= len(T_95):
        raise GateError(24, "t table requires df 1 through 60")
    return T_95[degrees - 1]


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise GateError(24, "no frame intervals")
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def present_event(event):
    values = tuple(event.get(name) for name in PRESENT_FIELDS)
    if any(type(value) is not int or value < 0 for value in values):
        raise GateError(24, "present needs nonnegative integer n, t_ns and driver counters")
    n, timestamp, attempts, presented, acquire_dropped, out_of_date = values
    if presented + acquire_dropped + out_of_date > attempts:
        raise GateError(24, "inconsistent presentation counters")
    return values


def validate_events(events):
    for previous, current in zip(events, events[1:]):
        if current[0] != previous[0] + 1 or current[1] <= previous[1]:
            raise GateError(24, "present gap, duplicate or non-monotonic timestamp")
        delta = [new - old for old, new in zip(previous[2:], current[2:])]
        if delta[0] < 1 or any(value < 0 for value in delta) or sum(delta[1:]) > delta[0]:
            raise GateError(24, "non-monotonic or inconsistent presentation counters")


def leg_summary(path, start_ns, end_ns, hold_seconds):
    if hold_seconds <= 0 or end_ns <= start_ns:
        raise GateError(24, "invalid hold window")
    events = []
    with Path(path).open(encoding="utf-8-sig") as source:
        for line in source:
            try:
                event = json.loads(line)
            except (ValueError, TypeError) as error:
                raise GateError(24, f"invalid telemetry: {error}") from error
            if not isinstance(event, dict):
                raise GateError(24, "telemetry event must be an object")
            if event.get("ev") != "present":
                continue
            values = present_event(event)
            if start_ns <= values[1] <= end_ns:
                events.append(values)
    return summarize_events(events, start_ns, end_ns, hold_seconds)


def summarize_events(events, start_ns, end_ns, hold_seconds):
    if len(events) < 2:
        raise GateError(24, "insufficient full-rate telemetry")
    validate_events(events)
    attempts, presented, acquire_dropped, out_of_date = [last - first for first, last in zip(events[0][2:], events[-1][2:])]
    if presented < 2:
        raise GateError(24, "fewer than two presented frames")
    intervals = []
    last_present_ns = events[0][1]
    for previous, current in zip(events, events[1:]):
        if current[3] > previous[3]:
            intervals.append((current[1] - last_present_ns) / 1e6)
            last_present_ns = current[1]
    span = (events[-1][1] - events[0][1]) / 1e9
    if span < 0.9 * hold_seconds:
        raise GateError(24, f"only {span:.3f}s of a {hold_seconds}s hold")
    ms = (events[-1][1] - events[0][1]) / presented / 1e6
    dropped = attempts - presented
    return {"code": 27 if dropped * 100 > attempts else 0,
            "verdict": "present_drops" if dropped * 100 > attempts else "valid",
            "attempts": attempts, "presented": presented, "dropped": dropped, "drop_rate": dropped / attempts,
            "acquire_dropped": acquire_dropped, "out_of_date": out_of_date,
            "ms": ms, "fps": 1000 / ms, "p50": percentile(intervals, 0.5),
            "p95": percentile(intervals, 0.95), "p99": percentile(intervals, 0.99),
            "longest_interval_ms": max(intervals), "frames": len(events), "span_s": span,
            "start_ns": start_ns, "end_ns": end_ns}


def positive(value):
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise GateError(24, "frame metrics must be finite and positive")
    return value


def pair_deltas(pairs):
    return [math.log(positive(pair["candidate"]["ms"]) / positive(pair["control"]["ms"])) for pair in pairs]


def memory_report(configurations):
    pressure = {role: sum(leg.get("min_available_memory") is not None and leg["min_available_memory"] < 512 * 1024 * 1024
                          for leg in legs) for role, legs in configurations.items()}
    return {"memory_pressure_blocks": pressure, "memory_pressure": any(pressure.values()),
            "verdict_notes": [f"Memory pressure below 512 MiB affected {count} {role} measured blocks."
                              for role, count in pressure.items() if count]}


def measurement_report(configurations, baseline=None):
    drops = {}
    invalid = False
    for role, legs in configurations.items():
        fields = ("attempts", "presented", "acquire_dropped", "out_of_date")
        for leg in legs:
            if any(type(leg.get(name)) is not int or leg[name] < 0 for name in fields):
                raise GateError(24, "leg needs nonnegative integer presentation counters")
            if leg["presented"] < 2 or sum(leg[name] for name in fields[1:]) > leg["attempts"]:
                raise GateError(24, "insufficient or inconsistent presented frames")
        totals = {name: sum(leg[name] for leg in legs) for name in fields}
        invalid = invalid or any((leg["attempts"] - leg["presented"]) * 100 > leg["attempts"] for leg in legs)
        totals["dropped"] = totals["attempts"] - totals["presented"]
        totals["drop_rate"] = totals["dropped"] / totals["attempts"] if totals["attempts"] else 0
        drops[role] = totals
    report = {"drops": drops, "candidate_mean_ms": statistics.mean(positive(leg["ms"]) for leg in configurations["candidate"]),
              **memory_report(configurations)}
    if baseline is not None:
        report.update(baseline_mean_ms=positive(baseline["mean_ms"]))
        report["cumulative_speedup"] = report["baseline_mean_ms"] / report["candidate_mean_ms"]
        report["cumulative_change"] = report["candidate_mean_ms"] / report["baseline_mean_ms"] - 1
    if invalid:
        report.update(code=27, verdict="present_drops")
    return report


def pair_report(pairs, baseline=None):
    report = measurement_report({role: [pair[role] for pair in pairs] for role in ("control", "candidate")}, baseline)
    report["pair_min_available_memory"] = [min((pair[role]["min_available_memory"] for role in ("control", "candidate")
                                              if pair[role].get("min_available_memory") is not None), default=None) for pair in pairs]
    return report


def bootstrap(pairs, alternating=False):
    if len(pairs) != (24 if alternating else 6):
        raise GateError(24, "Bootstrap requires 24 alternating or six boot pairs")
    deltas = pair_deltas(pairs)
    noise = statistics.stdev(deltas)
    noisy = t_critical(47) * noise / math.sqrt(48) > 0.01
    mean_ms = statistics.mean(positive(leg["ms"]) for pair in pairs for leg in pair.values())
    result = {"code": 40 if noisy else 0, "verdict": "noise" if noisy else "pass",
              "d_i": deltas, "s_AA": noise, "m": 0.02, "stage": len(pairs), "mean_ms": mean_ms,
              **pair_report(pairs, {"mean_ms": mean_ms})}
    if alternating:
        result["alternating_m"] = 0.02
    return result


def sequential(pairs, margin=0.02, alternating=False):
    stages = (12, 24, 48) if alternating else (4, 8)
    if len(pairs) not in stages:
        raise GateError(24, f"Child requires {stages} pairs")
    margin = 0.02
    deltas = pair_deltas(pairs)
    mean = statistics.mean(deltas)
    critical = t_critical(len(pairs) - 1) if alternating else {4: 2.353, 8: 1.895}[len(pairs)]
    radius = critical * statistics.stdev(deltas) / math.sqrt(len(pairs))
    lower, upper = mean - radius, mean + radius
    threshold = math.log1p(margin)
    decision = "pass" if upper <= threshold else "fail" if lower > threshold else "inconclusive" if len(pairs) == stages[-1] else "continue"
    return {"code": {"pass": 0, "continue": 0, "fail": 1, "inconclusive": 3}[decision], "verdict": decision,
            "d_i": deltas, "LB": lower, "UB": upper, "threshold": threshold, "m": margin,
            "stage": len(pairs), "speedup": math.exp(-mean),
            "speedup_90_interval": [math.exp(-upper), math.exp(-lower)], "sequential_rule": FALSE_PASS_NOTE,
            **pair_report(pairs)}


def alternating_blocks(telemetry, text, blocks, settle_seconds):
    if not math.isfinite(settle_seconds) or settle_seconds < 0 or len(blocks) not in (24, 48, 96):
        raise GateError(24, "alternation needs 24, 48 or 96 blocks and nonnegative settling")
    toggles = {}
    last_sequence, last_timestamp = 0, -1
    for line in text.splitlines():
        if line.startswith("[archb] toggle refused"):
            raise GateError(21, line)
        match = re.fullmatch(r"\[archb\] toggle seq=([0-9]+) t_ns=([0-9]+)((?: [A-Z][A-Z0-9_]*=[0-9]+)+)", line)
        if match is None:
            continue
        sequence, timestamp = int(match[1]), int(match[2])
        if sequence <= last_sequence or timestamp <= last_timestamp:
            raise GateError(24, "non-monotonic toggle acknowledgement")
        last_sequence, last_timestamp = sequence, timestamp
        toggles[sequence] = {"t_ns": timestamp, "values": dict(re.findall(r"([A-Z][A-Z0-9_]*)=([0-9]+)", match[3]))}
    data = Path(telemetry).read_bytes()
    events = []
    for line in data[:data.rfind(b"\n") + 1].splitlines():
        event = json.loads(line)
        if not isinstance(event, dict):
            raise GateError(24, "telemetry event must be an object")
        if event.get("ev") == "present":
            events.append(present_event(event))
    summaries = []
    previous_end = None
    for block in blocks:
        start, end = toggles.get(block["start_seq"]), toggles.get(block["end_seq"])
        if start is None or end is None or block["end_seq"] <= block["start_seq"]:
            raise GateError(24, "missing block boundary acknowledgement")
        if any(start["values"].get(name) != value for name, value in block["values"].items()):
            raise GateError(21, "toggle values differ from block plan")
        if previous_end is not None and start["t_ns"] < previous_end:
            raise GateError(24, "overlapping blocks")
        previous_end = end["t_ns"]
        settled = start["t_ns"] + round(settle_seconds * 1e9)
        duration = (end["t_ns"] - settled) / 1e9
        if duration <= 0:
            raise GateError(24, "settling consumes block")
        selected = [event for event in events if settled <= event[1] < end["t_ns"]]
        summary = summarize_events(selected, settled, end["t_ns"], duration)
        minimum = block.get("min_available_memory")
        summaries.append({**block, **summary, "toggle_ns": start["t_ns"],
                          "memory_pressure": minimum is not None and minimum < 512 * 1024 * 1024})
    window = [event for event in events if summaries[0]["toggle_ns"] <= event[1] < previous_end]
    validate_events(window)
    pairs = []
    for index in range(0, len(summaries), 2):
        pair = {block["role"]: block for block in summaries[index:index + 2]}
        if set(pair) != {"control", "candidate"}:
            raise GateError(24, "adjacent blocks must be a control/candidate pair")
        pairs.append(pair)
    return pairs


def alternating_verdict(directory, settle_seconds, baseline=None, calibration=False):
    if not calibration and baseline is None:
        raise GateError(24, "alternating gate requires a frozen baseline")
    directory = Path(directory)
    pairs = alternating_blocks(directory / "videoout.jsonl", (directory / "stderr.log").read_text(encoding="utf-8", errors="replace"),
                               read_json(directory / "blocks.json"), settle_seconds)
    decision = bootstrap(pairs, True) if calibration else sequential(pairs, alternating=True)
    if not calibration:
        decision.update(pair_report(pairs, baseline))
    decision.update(tail_check(pairs))
    if decision["code"] == 0 and decision["verdict"] != "continue" and decision["tail_code"]:
        decision.update(code=2, verdict="fail")
    return {**decision, "pairs": pairs, "alternating": True}


def tail_check(pairs):
    ratios = [positive(pair["candidate"]["p95"]) / positive(pair["control"]["p95"]) for pair in pairs]
    median = statistics.median(ratios)
    return {"tail_ratios": ratios, "tail_median": median, "tail_code": 0 if median <= 1.05 else 2}


def final_verdict(legs, baseline=None):
    if len(legs) != 3:
        raise GateError(24, "Final requires three candidate legs")
    passed = all(positive(leg["ms"]) <= 1000 / 15.0 for leg in legs)
    return {"code": 0 if passed else 1, "verdict": "pass" if passed else "fail", "legs": legs, "stage": 3, "m": 0.02,
            **measurement_report({"candidate": legs}, baseline)}


def recorder_diagnostics(directory):
    directory = Path(directory)
    path = directory / "aps5-summary.jsonl"
    if not path.is_file():
        return None
    records = [item["archb"] for line in path.read_text(encoding="utf-8").splitlines()
               if "archb" in (item := json.loads(line))]
    if not records:
        return None
    window = read_json(directory / "hold.json") if (directory / "hold.json").is_file() else None
    config = read_json(directory / "recorder-config.json") if (directory / "recorder-config.json").is_file() else {}
    blocks = read_json(directory / "blocks.json") if (directory / "blocks.json").is_file() else None
    selected = []
    toggles = {}
    if blocks is not None:
        text = (directory / "stderr.log").read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"^\[archb\] toggle seq=(\d+) t_ns=(\d+)((?: [A-Z][A-Z0-9_]*=[0-9]+)+)$", text, re.M):
            toggles[int(match[1])] = (int(match[2]), dict(re.findall(r"([A-Z][A-Z0-9_]*)=([0-9]+)", match[3])))
    for record in records:
        if record.get("recorder_toggled", True) or not record.get("recorder_enabled", False):
            continue
        start, end = record["start_ns"], record["end_ns"]
        if end <= start or record["frames"] <= 0:
            continue
        if window is not None and (start < window["start_ns"] or end > window["end_ns"]):
            continue
        if blocks is not None:
            block = next((block for block in blocks if block["role"] == "candidate"
                          and block["start_seq"] == record["toggle_seq"]), None)
            if block is None:
                continue
            begin, finish = toggles.get(block["start_seq"]), toggles.get(block["end_seq"])
            if begin is None or finish is None or any(begin[1].get(name) != value for name, value in block["values"].items()):
                raise GateError(21, "recorder candidate toggle differs from block plan")
            if any(begin[1].get(name) != value for name, value in config.get("switches", {}).items()):
                raise GateError(21, "recorder candidate configuration differs from manifest")
            if start < begin[0] + round(config.get("settle_seconds", 3) * 1e9) or end > finish[0]:
                continue
        selected.append(record)
    frames = sum(item["frames"] for item in selected)
    if not frames:
        raise GateError(24, "no complete stable recorder intervals in the candidate measurement")
    phases, counters, shaders = {}, {}, {}
    accounted, unaccounted = 0.0, 0.0
    for item in selected:
        for role, rows in item["phases_ms"].items():
            for name, value in rows.items():
                key = role + ":" + name
                phases[key] = phases.get(key, 0.0) + value / frames
        for name, value in item["counters"].items():
            if not isinstance(value, (float, int)) or not math.isfinite(value) or value < 0:
                raise GateError(24, "invalid recorder counter " + name)
            counters[name] = counters.get(name, 0.0) + value
        for row in item["shaders"]["top"]:
            key = row["kind"] + ":" + row["code"]
            shaders[key] = shaders.get(key, 0.0) + sum(row[name] for name in ("prepare_ms", "record_ms", "proof_ms", "bind_ms")) / frames
        accounted += item["accounted_cpu_ms_per_frame"] * item["frames"] / frames
        unaccounted += item["unaccounted_ms_per_frame"] * item["frames"] / frames
    missing = [name for name in config.get("requiredCounters", []) if counters.get(name, 0) <= 0]
    if missing:
        raise GateError(22, "required recorder counters missing or zero: " + ", ".join(missing))
    ledger = config.get("allocatorLedgerBytes")
    if ledger is not None:
        if "allocator_high_water_bytes" not in counters or counters["allocator_high_water_bytes"] > ledger:
            raise GateError(26, "recorder allocator high-water is missing or exceeds its ledger")
    return {"source": "recorder", "frames": frames, "window": window, "recorder_intervals": selected,
            "counters": counters, "shader_exclusive_cpu": shaders, "archb_ms_per_frame": phases,
            "accounted_cpu_ms_per_frame": accounted, "unaccounted_ms_per_frame": unaccounted}


def diagnostics(directory):
    recorder = recorder_diagnostics(directory)
    if recorder is not None:
        return recorder
    directory = Path(directory)
    window = read_json(directory / "hold.json") if (directory / "hold.json").is_file() else None
    with (directory / "stderr.log").open("rb") as source:
        if window is not None:
            source.seek(window["stderr_start"])
            text = source.read(window["stderr_end"] - window["stderr_start"]).decode("utf-8", errors="replace")
        else:
            text = source.read().decode("utf-8", errors="replace")
    frames = 0
    first_present_ns = None
    if (directory / "videoout.jsonl").is_file():
        for line in (directory / "videoout.jsonl").read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event.get("ev") == "present":
                if first_present_ns is None:
                    first_present_ns = event["t_ns"]
                if window is None or window["start_ns"] <= event["t_ns"] <= window["end_ns"]:
                    frames += 1
    if frames == 0:
        raise GateError(24, "diagnostic telemetry has no present events")
    denominator = frames
    shader_lines = []
    shaders = {}
    shader_kind = None
    for line in text.splitlines():
        header = re.match(r"\[(draw|dispatch)-shaders\]", line)
        if header:
            shader_kind = header[1]
            shader_lines.append(line)
            continue
        if line.startswith("["):
            shader_kind = None
        if shader_kind is None or not line.startswith("  "):
            continue
        shader_lines.append(line)
        row = re.match(r"\s+(.+?): .*? packets.*?, ([0-9.]+) ms", line)
        if row:
            key = shader_kind + ":" + row[1]
            shaders[key] = shaders.get(key, 0.0) + float(row[2]) / denominator
    phases = {}
    packet_ms = 0.0
    phase_lines = [line for line in text.splitlines() if "[archb]" in line and "switches" not in line]
    for line in phase_lines:
        for name, value in re.findall(r"\b([A-Za-z_][\w.-]*_ms)\s*[=:]\s*([0-9.]+)", line):
            if name == "queue0_packet_ms":
                packet_ms += float(value)
            else:
                phases[name] = phases.get(name, 0.0) + float(value) / denominator
    packets = [line for line in text.splitlines() if re.search(r"\[packets\] queue 0x0\b", line)]
    scopes = {}
    summaries = []
    path = directory / "aps5-summary.jsonl"
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            item = json.loads(line)
            if "interval_start_s" not in item:
                continue
            if window is not None:
                interval_start = first_present_ns + round(item["interval_start_s"] * 1e9)
                interval_end = first_present_ns + round(item["interval_end_s"] * 1e9)
                if interval_start < window["start_ns"] or interval_end > window["end_ns"]:
                    continue
            summaries.append(item)
            for name, milliseconds in item.get("top_phases_ms_per_frame", {}).items():
                scopes.setdefault(name, []).append({"ms_per_frame": milliseconds, "presents": item.get("presents", 0),
                                                   "total_ms": milliseconds * item.get("presents", 0)})
    return {"frames": frames, "window": window, "shader_exclusive_cpu": shaders, "shader_lines": shader_lines,
            "archb_ms_per_frame": phases, "archb_lines": phase_lines, "trace_scopes": scopes,
            "trace_summaries": summaries, "queue0_packet_lines": packets,
            "queue0_packet_ms_per_frame": packet_ms / denominator,
            "accounted_cpu_ms_per_frame": packet_ms / denominator + sum(phases.values())}


UB_SWITCHES = (
    "UB_TRUST_PROOFS", "UB_NO_LOCK_INVALIDATION", "UB_POINTER_FREE_DRAW_KEYS",
    "UB_SKIP_CPU_INDIRECT", "UB_SKIP_DISPATCH_PROOF_WALK",
)


def ub_counter_hits(text, switches):
    return {name.lower() + "_hits": max((int(value) for value in re.findall(
        rf"\b{re.escape(name.lower())}_hits=([0-9]+)\b", text)), default=0) for name in switches}


def ub_recorder_attribution(directory, pairs):
    path = Path(directory) / "aps5-summary.jsonl"
    if not path.is_file():
        return {}
    with (Path(directory) / "videoout.jsonl").open(encoding="utf-8-sig") as source:
        first_present = next(json.loads(line)["t_ns"] for line in source if json.loads(line).get("ev") == "present")
    result = {role: {"frames": 0, "intervals": 0, "phases_ms_per_frame": {}} for role in ("control", "candidate")}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        item = json.loads(line)
        record = item.get("archb")
        if not record or item.get("recorder_toggled") or record.get("recorder_toggled") or record.get("recorder_enabled") is False:
            continue
        start = record.get("start_ns", item.get("interval_start_ns"))
        end = record.get("end_ns", item.get("interval_end_ns"))
        if start is None or end is None:
            start = first_present + round(item["interval_start_s"] * 1e9)
            end = first_present + round(item["interval_end_s"] * 1e9)
        for role in result:
            if not any(pair[role]["start_ns"] <= start < end <= pair[role]["end_ns"] for pair in pairs):
                continue
            frames = record.get("presented", record.get("frames", 0))
            if frames <= 0:
                continue
            target = result[role]
            target["frames"] += frames
            target["intervals"] += 1
            for thread, phases in record.get("phases_ms", {}).items():
                if isinstance(phases, dict):
                    totals = target["phases_ms_per_frame"].setdefault(thread, {})
                    for phase, milliseconds in phases.items():
                        totals[phase] = totals.get(phase, 0) + milliseconds
                else:
                    totals = target["phases_ms_per_frame"]
                    totals[thread] = totals.get(thread, 0) + phases
    for target in result.values():
        if target["frames"]:
            for thread, phases in target["phases_ms_per_frame"].items():
                if isinstance(phases, dict):
                    target["phases_ms_per_frame"][thread] = {phase: milliseconds / target["frames"] for phase, milliseconds in phases.items()}
                else:
                    target["phases_ms_per_frame"][thread] /= target["frames"]
    return result


def ub_experiment(directory, name, switches, settle_seconds=3):
    if not switches or set(switches) - set(UB_SWITCHES):
        raise GateError(21, "unknown upper-bound experiment")
    directory = Path(directory)
    text = (directory / "stderr.log").read_text(encoding="utf-8", errors="replace")
    pairs = alternating_blocks(directory / "videoout.jsonl", text, read_json(directory / "blocks.json"), settle_seconds)
    deltas = pair_deltas(pairs)
    mean = statistics.mean(deltas)
    radius = t_critical(len(pairs) - 1) * statistics.stdev(deltas) / math.sqrt(len(pairs))
    fps = {}
    for role in ("control", "candidate"):
        intervals = sum(pair[role].get("presented", pair[role]["frames"] - 1) for pair in pairs)
        span = sum(pair[role]["span_s"] for pair in pairs)
        fps[role] = intervals / positive(span)
    return {"name": name, "switches": list(switches), "status": "completed", "stage": "measurement",
            "frame_time_ratio": math.exp(mean), "frame_time_ratio_90_interval": [math.exp(mean - radius), math.exp(mean + radius)],
            "presented_fps": fps, "hits": ub_counter_hits(text, switches), "d_i": deltas, "pairs": pairs,
            **memory_report({role: [pair[role] for pair in pairs] for role in ("control", "candidate")}),
            "attribution": ub_recorder_attribution(directory, pairs), "directory": str(directory)}


def ub_report(experiments):
    rows = ["| Experiment | Status / stage | Candidate/control ms (90% interval) | Control FPS | Candidate FPS | Hits |",
            "|---|---|---:|---:|---:|---:|"]
    for item in experiments:
        if item["status"] == "completed":
            low, high = item["frame_time_ratio_90_interval"]
            metric = f"{item['frame_time_ratio']:.4f} [{low:.4f}, {high:.4f}]"
            control, candidate = (f"{item['presented_fps'][role]:.3f}" for role in ("control", "candidate"))
        else:
            metric, control, candidate = "n/a", "n/a", "n/a"
        hits = ", ".join(f"{name}={value}" for name, value in item.get("hits", {}).items()) or "0"
        rows.append(f"| {item['name']} | {item['status']} / {item['stage']} | {metric} | {control} | {candidate} | {hits} |")
    return {"format": "anyps5-ub/1", "experiments": experiments, "markdown": "\n".join(rows) + "\n"}


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    leg = sub.add_parser("leg")
    leg.add_argument("path")
    leg.add_argument("--start-ns", type=int, required=True)
    leg.add_argument("--end-ns", type=int, required=True)
    leg.add_argument("--hold-seconds", type=float, required=True)
    decide = sub.add_parser("decide")
    decide.add_argument("pairs")
    decide.add_argument("--mode", choices=("Bootstrap", "Child", "Milestone", "Final"), required=True)
    decide.add_argument("--baseline")
    diagnostic = sub.add_parser("diagnostic")
    diagnostic.add_argument("directory")
    alternate = sub.add_parser("alternating")
    alternate.add_argument("directory")
    alternate.add_argument("--settle-seconds", type=float, default=3)
    alternate.add_argument("--baseline")
    alternate.add_argument("--bootstrap", action="store_true")
    experiment = sub.add_parser("ub-experiment")
    experiment.add_argument("directory")
    experiment.add_argument("--name", required=True)
    experiment.add_argument("--switch", action="append", required=True)
    experiment.add_argument("--settle-seconds", type=float, default=3)
    report = sub.add_parser("ub-report")
    report.add_argument("experiments")
    report.add_argument("--table")
    for command in (leg, decide, diagnostic, alternate, experiment, report):
        command.add_argument("--output")
    args = parser.parse_args()
    try:
        if args.command == "ub-experiment":
            result = ub_experiment(args.directory, args.name, args.switch, args.settle_seconds)
        elif args.command == "ub-report":
            result = ub_report(read_json(args.experiments))
            if args.table:
                Path(args.table).write_text(result["markdown"], encoding="utf-8")
        elif args.command == "leg":
            result = leg_summary(args.path, args.start_ns, args.end_ns, args.hold_seconds)
        elif args.command == "diagnostic":
            result = diagnostics(args.directory)
        elif args.command == "alternating":
            result = alternating_verdict(args.directory, args.settle_seconds,
                                         read_json(args.baseline) if args.baseline else None, args.bootstrap)
        elif args.mode == "Final":
            result = final_verdict(read_json(args.pairs), read_json(args.baseline))
        else:
            pairs = read_json(args.pairs)
            result = bootstrap(pairs) if args.mode == "Bootstrap" else sequential(pairs, read_json(args.baseline)["m"])
            if args.mode != "Bootstrap":
                result.update(pair_report(pairs, read_json(args.baseline)))
            result.update(tail_check(pairs))
            if result["code"] == 0 and result["verdict"] != "continue" and result["tail_code"]:
                result.update(code=2, verdict="fail")
        if args.output:
            write_json(args.output, result)
        print(json.dumps(result))
        return result.get("code", 0)
    except (GateError, OSError, ValueError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return error.code if isinstance(error, GateError) else 24


if __name__ == "__main__":
    sys.exit(main())
