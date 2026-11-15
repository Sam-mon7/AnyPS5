import argparse
import bisect
import collections
import gzip
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from critical_path import read_trace


def summarize(trace):
    if not trace["complete"] or trace["loss"] or trace["dropped"] or trace["cap_reached"] or trace["cap_dropped"] or trace["name_loss"] or trace["unregistered_events"]:
        raise ValueError("dispatch timing requires a complete trace without event or name loss")
    calls = []
    incomplete = 0
    for tid, timeline in trace["threads"].items():
        stack = []
        active = []
        previous = None
        for event in timeline:
            if previous is not None and active:
                call = active[-1]
                leaf = stack[-1][1] if stack else "UNINSTRUMENTED"
                call["phase_ns"][leaf] += max(0, event["ns"] - previous["ns"])
            if event["kind"] == "phase_begin":
                stack.append((event["id"], event["name"]))
                if event["name"] in {"Driver.Dispatch", "Driver.DispatchCommit"}:
                    active.append(dict(id=event["id"], tid=tid, kind=event["name"], start_ns=event["ns"], phase_ns=collections.Counter(), points=[]))
            elif event["kind"] == "phase_end":
                if stack and stack[-1][0] == event["value"]:
                    stack.pop()
                    if active and active[-1]["id"] == event["value"]:
                        call = active.pop()
                        call["end_ns"] = event["ns"]
                        calls.append(call)
                else:
                    if active:
                        raise ValueError("phase mismatch inside a dispatch invocation")
                    active.clear()
                    stack.clear()
            elif event["kind"] == "point" and event["name"].startswith("dispatch.") and active:
                active[-1]["points"].append(dict(id=event["id"], ns=event["ns"], name=event["name"], object=event["object"], value=event["value"]))
            previous = event
        incomplete += len(active)
    sites = {}
    frame_times = [event["ns"] for event in trace.get("presents", [])]
    metadata_counts = collections.Counter()
    for call in sorted(calls, key=lambda item: (item["start_ns"], item["tid"], item["id"])):
        points = call["points"]
        site = next((p for p in points if p["name"] in {"dispatch.site", "dispatch.commit.site"}), None)
        if site is None:
            continue
        resolving = next((p["value"] for p in points if p["name"] == "dispatch.resolving"), 0)
        indirect = next((p["value"] for p in points if p["name"] == "dispatch.entry.indirect"), 0)
        identity = (call["tid"], site["value"], site["object"], call["kind"], resolving, indirect)
        if identity not in sites:
            sites[identity] = dict(thread=trace["names"].get(call["tid"], str(call["tid"])), queue=site["value"], program=hex(site["object"]), kind=call["kind"], resolving=bool(resolving), indirect=bool(indirect), calls=0, repeated_keys=0, repeated_keys_prior_frame=0, keys={}, prior_components=None, component_changes=collections.Counter(), outcomes=collections.Counter(), reason_values=collections.defaultdict(collections.Counter), recipe_failures=collections.Counter(), phases=collections.Counter())
        row = sites[identity]
        row["calls"] += 1
        row["phases"].update(call["phase_ns"])
        key = next((p["value"] for p in points if p["name"] == "dispatch.key"), None)
        if key is not None:
            frame = bisect.bisect_right(frame_times, call["start_ns"]) if frame_times else None
            row["repeated_keys"] += key in row["keys"]
            row["repeated_keys_prior_frame"] += key in row["keys"] and frame is not None and row["keys"][key] < frame
            row["keys"].setdefault(key, frame)
            components = {name: [(p["object"], p["value"]) for p in points if p["name"] == name] for name in ("dispatch.shader", "dispatch.wave", "dispatch.partial", "dispatch.register", "dispatch.user_word", "dispatch.key_policy")}
            if row["prior_components"] is not None:
                for name, values in components.items():
                    row["component_changes"][name] += values != row["prior_components"][name]
            row["prior_components"] = components
        for point in points:
            name = point["name"]
            metadata_counts[name] += 1
            if name.startswith(("dispatch.cache.", "dispatch.no_recipe.", "dispatch.hle.", "dispatch.lookup.", "dispatch.insert.", "dispatch.evict.", "dispatch.relocation_recipe.")) or name == "dispatch.skipped":
                row["outcomes"][name] += 1
            if name in {"dispatch.validation", "dispatch.validation.final", "dispatch.validation.current", "dispatch.precheck", "dispatch.precheck.miss", "dispatch.precheck.cause", "dispatch.recipe.precheck_miss", "dispatch.cache.result", "dispatch.relocation_recipe.missing", "dispatch.relocation_recipe.validation", "dispatch.relocation_recipe.candidate"}:
                row["reason_values"][name][str(point["value"])] += 1
            if name == "dispatch.recipe.build_failure":
                row["recipe_failures"][str(point["value"])] += 1
    result = []
    for row in sites.values():
        row["unique_keys"] = len(row.pop("keys"))
        row.pop("prior_components")
        row["exclusive_wall_ms"] = sum(row["phases"].values()) / 1e6
        row["phase_ms"] = {name: value / 1e6 for name, value in row.pop("phases").most_common()}
        result.append(row)
    return dict(format="aps5-dispatch-timing/1", measurement="whole-thread exclusive elapsed spans, not selected critical-path time or CPU execution", key_repeat_evidence="key equality in this capture only, not unchanged consumed bytes or reuse proof; component_changes compares successive invocations at each site/mode", metadata_counts=metadata_counts, capture_window=trace.get("window"), complete_calls=len(calls), incomplete_calls=incomplete, site_missing_calls=len(calls)-sum(row["calls"] for row in result), warnings=trace["warnings"], sites=sorted(result, key=lambda row: -row["exclusive_wall_ms"]), calls=calls)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--calls", action="store_true")
    args = parser.parse_args()
    opener = gzip.open if args.trace.suffix == ".gz" else open
    with opener(args.trace, "rt", encoding="utf-8") as source:
        report = summarize(read_trace(source))
    if not args.calls:
        report.pop("calls")
    json.dump(report, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
