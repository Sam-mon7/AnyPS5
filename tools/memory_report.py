import argparse
import json
from pathlib import Path


def mib(value):
    return "n/a" if value is None else f"{value / 1048576:,.1f}"


def report(records):
    capture = next((row for row in records if row["type"] == "capture"), None)
    if capture:
        print(f"Captured virtual ranges: {mib(capture['range_address_bytes'])} MiB")
        print("Range kinds: " + ", ".join(f"{key} {mib(value)} MiB" for key, value in capture["range_kinds"].items() if value))
        print(f"Capture index capacity: {mib(capture['index_capacity_bytes'])} MiB")
        print(f"Disk payload: {mib(capture['stored_payload_bytes'])} MiB compressed; snapshot {mib(capture['snapshot_payload_bytes'])} MiB decoded")
    checkpoints = [row for row in records if row["type"] == "checkpoint"]
    if not checkpoints:
        return
    print("\nMiB at checkpoints (overlapping measurements; do not add columns):")
    print(f"{'Stage':24} {'RAM':>11} {'Private':>11} {'Peak RAM':>11} {'Local heap':>11} {'Other heap':>11} {'Free buffers':>12} {'Free images':>12}")
    for row in checkpoints:
        process, driver = row["process"], row["driver"]
        heaps = driver["heaps"]
        def heap_usage(local):
            selected = [heap["usage_estimate_bytes"] for heap in heaps if heap["device_local"] == local]
            return sum(selected) if selected and all(value is not None for value in selected) else None
        label = row["stage"] + (f" {row['pass']}" if row["pass"] else "")
        values = [process["resident_bytes"], process["private_commit_bytes"], process["peak_resident_bytes"], heap_usage(True), heap_usage(False), driver["retained_buffer_bytes"] if driver["queried"] else None, driver["retained_image_bytes"] if driver["queried"] else None]
        print(f"{label:24}" + "".join(f" {mib(value):>11}" for value in values))
    ends = [row for row in checkpoints if row["stage"] == "pass_end"]
    if len(ends) > 1:
        first, last = ends[0], ends[-1]
        print(f"\nRetained growth after pass {first['pass']} to pass {last['pass']} (MiB):")
        for group, keys in (("process", ("resident_bytes", "private_commit_bytes", "guest_arena_private_bytes", "host_private_region_bytes")), ("driver", ("retained_buffer_bytes", "retained_image_bytes", "slab_allocated_bytes", "imported_guest_bytes", "open_kept_bytes", "in_flight_kept_bytes"))):
            for key in keys:
                before, after = first[group][key], last[group][key]
                if before is not None and after is not None:
                    print(f"  {key.removesuffix('_bytes')}: {(after - before) / 1048576:+,.1f}")
    last = checkpoints[-1]["process"]
    if last["guest_arena_private_bytes"] is not None:
        print(f"\nFinal committed private regions: guest arena {mib(last['guest_arena_private_bytes'])} MiB; outside arena {mib(last['host_private_region_bytes'])} MiB.")
        print("These are virtual committed regions, not resident RAM; the arena can include runtime allocations.")
    print("\nRAM is resident memory; Private is Windows private commit. Heap usage is a driver estimate.")
    print("Guest imports alias guest RAM; slab storage overlaps pooled slots; kept bytes are estimates.")
    print("Growth can be cache warm-up; repeated growth alone does not prove a leak.")


def main():
    parser = argparse.ArgumentParser(description="Summarize agc_frame_replay --memory-report JSONL or --memory-summary JSON.")
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        records = [json.loads(line) for line in args.report.read_text(encoding="utf-8-sig").splitlines() if line.lstrip().startswith("{")]
        if not records or any(row.get("version") != 1 or row.get("type") not in ("capture", "checkpoint") for row in records):
            raise ValueError("expected memory report version 1 records")
        report(records)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
