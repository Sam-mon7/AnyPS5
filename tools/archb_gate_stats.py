import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

from archb_gate_config import GateError, read_json, write_json


FALSE_PASS_NOTE = "The fixed two-stage rule inflates the false-pass rate slightly above 5%; this is accepted."


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise GateError(24, "no frame intervals")
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


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
            n, timestamp = event.get("n"), event.get("t_ns")
            if type(n) is not int or type(timestamp) is not int:
                raise GateError(24, "present needs integer n and t_ns")
            if start_ns <= timestamp <= end_ns:
                events.append((n, timestamp))
    if len(events) < 2:
        raise GateError(24, "insufficient full-rate telemetry")
    intervals = []
    for (n0, t0), (n1, t1) in zip(events, events[1:]):
        if n1 != n0 + 1 or t1 <= t0:
            raise GateError(24, "present gap, duplicate or non-monotonic timestamp")
        intervals.append((t1 - t0) / 1e6)
    span = (events[-1][1] - events[0][1]) / 1e9
    if span < 0.9 * hold_seconds:
        raise GateError(24, f"only {span:.3f}s of a {hold_seconds}s hold")
    ms = (events[-1][1] - events[0][1]) / (events[-1][0] - events[0][0]) / 1e6
    return {"ms": ms, "fps": 1000 / ms, "p50": percentile(intervals, 0.5),
            "p95": percentile(intervals, 0.95), "p99": percentile(intervals, 0.99),
            "longest_interval_ms": max(intervals), "frames": len(events), "span_s": span,
            "start_ns": start_ns, "end_ns": end_ns}


def positive(value):
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise GateError(24, "frame metrics must be finite and positive")
    return value


def pair_deltas(pairs):
    return [math.log(positive(pair["candidate"]["ms"]) / positive(pair["control"]["ms"])) for pair in pairs]


def bootstrap(pairs):
    if len(pairs) != 6:
        raise GateError(24, "Bootstrap requires six pairs")
    deltas = pair_deltas(pairs)
    noise = statistics.stdev(deltas)
    return {"code": 40 if noise > 0.033 else 0, "verdict": "noise" if noise > 0.033 else "pass",
            "d_i": deltas, "s_AA": noise, "m": max(0.02, 1.5 * noise), "stage": 6}


def sequential(pairs, margin):
    if len(pairs) not in (4, 8) or not math.isfinite(margin) or margin < 0.02:
        raise GateError(24, "Child requires four or eight pairs and a frozen margin >= 0.02")
    deltas = pair_deltas(pairs)
    mean = statistics.mean(deltas)
    critical = {4: 2.353, 8: 1.895}[len(pairs)]
    radius = critical * statistics.stdev(deltas) / math.sqrt(len(pairs))
    lower, upper = mean - radius, mean + radius
    threshold = math.log1p(margin)
    decision = "pass" if upper <= threshold else "fail" if lower > threshold or len(pairs) == 8 else "continue"
    return {"code": 0 if decision in ("pass", "continue") else 1, "verdict": decision,
            "d_i": deltas, "LB": lower, "UB": upper, "threshold": threshold, "m": margin,
            "stage": len(pairs), "speedup": math.exp(-mean),
            "speedup_90_interval": [math.exp(-upper), math.exp(-lower)], "sequential_rule": FALSE_PASS_NOTE}


def tail_check(pairs):
    ratios = [positive(pair["candidate"]["p95"]) / positive(pair["control"]["p95"]) for pair in pairs]
    median = statistics.median(ratios)
    return {"tail_ratios": ratios, "tail_median": median, "tail_code": 0 if median <= 1.05 else 2}


def final_verdict(legs):
    if len(legs) != 3:
        raise GateError(24, "Final requires three candidate legs")
    passed = all(positive(leg["ms"]) <= 66.7 for leg in legs)
    return {"code": 0 if passed else 1, "verdict": "pass" if passed else "fail", "legs": legs, "stage": 3}


def diagnostics(directory):
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
    for command in (leg, decide, diagnostic):
        command.add_argument("--output")
    args = parser.parse_args()
    try:
        if args.command == "leg":
            result = leg_summary(args.path, args.start_ns, args.end_ns, args.hold_seconds)
        elif args.command == "diagnostic":
            result = diagnostics(args.directory)
        elif args.mode == "Final":
            result = final_verdict(read_json(args.pairs))
        else:
            pairs = read_json(args.pairs)
            result = bootstrap(pairs) if args.mode == "Bootstrap" else sequential(pairs, read_json(args.baseline)["m"])
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
