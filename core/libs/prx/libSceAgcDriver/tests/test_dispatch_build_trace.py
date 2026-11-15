import json
import os
import subprocess
import sys

PREFIX = "[dispatch-build] "


def run(executable, arguments, enabled, expect_rows=True):
    env = os.environ.copy()
    env["APS5_TRACE_DISPATCH_BUILDS"] = enabled
    env["APS5_VERIFY_RECIPE"] = "1"
    env.pop("APS5_PROFILE_DRAW", None)
    env.pop("APS5_NO_COMPLETION_RECIPE", None)
    process = subprocess.run([executable, *arguments], env=env, capture_output=True, text=True, timeout=25)
    output = process.stdout + process.stderr
    if process.returncode == 77:
        print(output)
        sys.exit(77)
    if process.returncode != 0:
        raise AssertionError(output)
    if "VUID-" in output or "was not found but was requested by env var VK_INSTANCE_LAYERS" in output:
        raise AssertionError(output)
    rows = [json.loads(line.split(PREFIX, 1)[1]) for line in output.splitlines() if PREFIX in line]
    if enabled != "1":
        assert not rows, "disabled diagnostic emitted events"
    elif expect_rows:
        assert rows, "enabled diagnostic emitted no events"
    return rows


move, driver = sys.argv[1:]
run(move, ["--build-trace-rate"], "0")
samples = run(move, ["--build-trace-rate"], "1")
assert [row["key_occurrence"] for row in samples if row["reason"] == "sampling-test"] == [1, 2, 4, 8, 16, 32, 64, 128, 256]

run(move, ["--build-trace-overflow"], "0")
overflow = run(move, ["--build-trace-overflow"], "1")
site_overflow = [row for row in overflow if row["site_aggregate_overflow"]]
key_overflow = [row for row in overflow if row["aggregate_overflow"]]
assert site_overflow and key_overflow
for row in site_overflow:
    assert row["site_aggregate_scope"] == "global-overflow" and row["site_occurrence"] is None
    assert row["site_complete_ns_total"] is None
    assert row["site_overflow_occurrence"] >= 1
    if row["reason"] != "queue-overflow-test":
        assert row["queue_aggregate_scope"] == "queue+category" and row["queue_occurrence"] >= 1
for row in key_overflow:
    assert row["key_aggregate_scope"] == "global-overflow" and row["key_occurrence"] is None
    assert row["key_complete_ns_total"] is None
    assert row["key_overflow_occurrence"] >= 1
category = [row for row in overflow if row["reason"] == "category-test"]
assert len(category) == 2 and len({row["aggregate_category_id"] for row in category}) == 2
assert all(row["queue_occurrence"] == 1 for row in category)
queued = [row for row in overflow if row["reason"] == "queue-overflow-test"]
assert any(row["queue"] == 0 and not row["queue_aggregate_overflow"] for row in queued)
assert any(row["queue_aggregate_overflow"] for row in queued)
for row in overflow:
    if row["reason"] not in ["overflow-test", "queue-overflow-test"]:
        continue
    for prefix in ["key", "site", "queue"]:
        if row[prefix + "_aggregate_scope"] == "global-overflow":
            assert row[prefix + "_occurrence"] is None and row[prefix + "_complete_ns_total"] is None
            assert row[prefix + "_overflow_complete_ns_total"] == row[prefix + "_overflow_occurrence"] * 7
        else:
            assert row[prefix + "_complete_ns_total"] == row[prefix + "_occurrence"] * 7


run(move, ["--build-trace-bytes"], "0", False)
run(move, ["--build-trace-bytes"], "1", False)
run(move, ["--completion-recipe"], "0")
rows = run(move, ["--completion-recipe"], "1")
assert any(row["event"] == "build" for row in rows), rows
assert any(row["recipe_built"] == "built" for row in rows if row["event"] == "build"), rows
assert any(row["event"] == "reuse-failed" and row["failed_check"] == "import-identity" for row in rows), rows
assert any(row["stage_a_ns"] > 0 and row["complete_ns"] > 0 for row in rows), rows
for row in rows:
    assert row["version"] == 2
    assert row["steady_ns"] > 0 and row["trace_elapsed_ns"] >= 0
    assert row["queue_aggregate_scope"] == "queue+category"
    if row["event"] == "build":
        assert all(row[name] in [0, 1] for name in ["address_table", "bda_read_only", "space_only", "copied_writes", "holds_lease"])
    assert row["frame"] is None
    assert row["upload_bytes"] == row["region_cpu_bytes"] + row["staging_in_gpu_bytes"]
    assert row["upload_scope"] == "guest-region-cpu+staging-in"
    assert row["key_occurrence"] >= 1
    assert row["stable_dispatch_id"] != 0
    for name in ["stage_a_ns", "complete_ns", "images_ns", "upload_ns", "bda_ns", "revalidate_ns"]:
        assert row[name] >= 0, row
    if row["failed_check"] == "import-identity":
        assert row["prior_identity"] != row["current_identity"], row
        assert row["prior_address"] is not None and row["current_address"] is not None, row
rows = run(driver, [], "1")
assert any(row["address_table"] == 1 and row["bda_read_only"] == 1 for row in rows), rows
assert any(row["code_digest"] is not None and row["submission"] is not None and row["packet_offset"] is not None for row in rows), rows
print("dispatch build diagnostic: disabled/enabled output, live byte checks, build timing, import failure and driver identity passed")
