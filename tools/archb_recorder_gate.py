import argparse
import json
import math
import statistics
import sys
from pathlib import Path

from archb_gate_config import GateError, read_json, write_json
from archb_gate_stats import alternating_blocks, t_critical, tail_check


def verdict(pairs):
    count = len(pairs)
    if count not in (12, 24, 48):
        raise GateError(24, "recorder overhead requires 12, 24 or 48 pairs")
    deltas = [math.log(pair["candidate"]["ms"] / pair["control"]["ms"]) for pair in pairs]
    mean = statistics.mean(deltas)
    radius = t_critical(count - 1) * statistics.stdev(deltas) / math.sqrt(count)
    lower, upper = mean - radius, mean + radius
    fallback = count == 48 and lower <= math.log1p(0.01) < upper <= math.log1p(0.02)
    decision = "pass" if upper <= math.log1p(0.01) or fallback else "fail" if lower > math.log1p(0.01) else "inconclusive" if count == 48 else "continue"
    result = {"code": {"pass": 0, "continue": 0, "fail": 1, "inconclusive": 3}[decision],
              "verdict": decision, "stage": count, "d_i": deltas, "UB": upper, "LB": lower,
              "upper_overhead_percent": 100 * math.expm1(upper), "accepted_at_two_percent": fallback,
              "margin": 0.01, "pairs": pairs, "alternating": True,
              "sequential_note": "Bounds are one-sided 95% at each of 12, 24 and 48 pairs; optional stopping is not multiplicity adjusted."}
    result.update(tail_check(pairs))
    if result["code"] == 0 and decision == "pass" and result["tail_code"]:
        result.update(code=2, verdict="fail")
    return result


def scheduling(directory, pairs, settle_seconds):
    path = Path(directory) / "aps5-summary.jsonl"
    if not path.is_file():
        raise GateError(24, "recorder scheduling check has no interval file")
    blocks = {block["start_seq"]: (role, block) for pair in pairs for role, block in pair.items()}
    names = ("finisher_handoffs", "finisher_inline", "draw_pipeline_commits")
    totals = {role: {"frames": 0, **dict.fromkeys(names, 0)} for role in ("control", "candidate")}
    for line in path.read_text(encoding="utf-8").splitlines():
        item = json.loads(line).get("archb")
        if not item or item["recorder_toggled"] or item["toggle_seq"] not in blocks:
            continue
        role, block = blocks[item["toggle_seq"]]
        if item["recorder_enabled"] != (role == "candidate"):
            raise GateError(21, "recorder state differs from overhead block")
        if item["start_ns"] < block["toggle_ns"] + round(settle_seconds * 1e9) or item["end_ns"] > block["end_ns"]:
            continue
        if item["frames"] <= 0 or item["end_ns"] <= item["start_ns"]:
            raise GateError(24, "invalid recorder scheduling interval")
        if any(name not in item["counters"] for name in names):
            raise GateError(22, "recorder scheduling counters are missing")
        totals[role]["frames"] += item["frames"]
        for name in names:
            value = item["counters"][name]
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise GateError(24, "invalid recorder scheduling counter " + name)
            totals[role][name] += value
    if any(value["frames"] == 0 for value in totals.values()):
        raise GateError(24, "no stable scheduling intervals for both recorder states")
    ratios = {}
    for name in names:
        left = totals["control"][name] / totals["control"]["frames"]
        right = totals["candidate"][name] / totals["candidate"]["frames"]
        ratios[name] = right / left if left else 1.0 if right == 0 else None
    return {"totals": totals, "per_frame_ratios": ratios,
            "pass": all(ratio is not None and abs(ratio - 1) <= 0.005 for ratio in ratios.values())}


def measure(directory, settle_seconds):
    directory = Path(directory)
    blocks = read_json(directory / "blocks.json")
    if len(blocks) not in (24, 48, 96):
        raise GateError(24, "recorder overhead requires 24, 48 or 96 blocks")
    text = (directory / "stderr.log").read_text(encoding="utf-8", errors="replace")
    pairs = []
    for offset in range(0, len(blocks), 48):
        pairs.extend(alternating_blocks(directory / "videoout.jsonl", text, blocks[offset:offset + 48], settle_seconds))
    result = verdict(pairs)
    result["scheduling"] = scheduling(directory, pairs, settle_seconds)
    if result["verdict"] == "pass" and not result["scheduling"]["pass"]:
        result.update(code=1, verdict="fail", reason="recorder scheduling counts differ by more than 0.5% per frame")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    parser.add_argument("--settle-seconds", type=float, default=3)
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        result = measure(args.directory, args.settle_seconds)
    except (GateError, OSError, ValueError, KeyError, TypeError) as error:
        result = {"code": error.code if isinstance(error, GateError) else 24, "verdict": "invalid", "reason": str(error)}
    if args.output:
        write_json(args.output, result)
    print(json.dumps(result))
    return result["code"]


if __name__ == "__main__":
    sys.exit(main())
