import contextlib
import gzip
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import critical_path as cp


def event(tid, seq, ns, kind="point", name="", object=0, value=0, peer=0):
    if not name:
        name = {"label_batch": "label.recorded.batch", "gpu_submit_begin": "gpu.host.submit.begin", "gpu_submit_end": "gpu.host.submit.end", "gpu_observed": "gpu.completion.cpu_observed"}.get(kind, name)
    return dict(type="event", id=tid << 48 | seq, tid=tid, ns=ns, kind=kind, name=name, object=object, value=value, peer=peer)


def capture(events, **header):
    metadata = dict(type="header", format=cp.SCHEMA, clock="steady_ns", clock_shared=True, coverage=["test"], ring_capacity=1000, max_threads=64)
    metadata.update(header)
    rows = [metadata] + events
    for tid in sorted({e["tid"] for e in events}):
        rows.append(dict(type="thread_stats", tid=tid, emitted=sum(e["tid"] == tid for e in events), dropped=0))
    rows.append(dict(type="footer", complete=True))
    return rows


def read(rows):
    return cp.read_trace(io.StringIO("\n".join(json.dumps(row) for row in rows)))


def fixture():
    return [event(1, 1, 0, "present", value=100), event(1, 2, 2_000_000, "wait_begin", "job", object=7), event(1, 3, 9_000_000, "wait_end", "job", object=7, value=1 << 48 | 2, peer=2 << 48 | 3), event(1, 4, 10_000_000, "present", value=101), event(2, 1, 0, "phase_begin", "worker"), event(2, 2, 3_000_000, "point"), event(2, 3, 8_000_000, "release", "job", object=7), event(2, 4, 9_000_000, "phase_end", "worker")]


class CriticalPathTests(unittest.TestCase):
    def result(self, events=None):
        return cp.analyze(read(capture(events if events is not None else fixture())), 0)

    def test_two_threads_exact_cause(self):
        frame = self.result()["frames"][0]
        self.assertEqual(frame["elapsed_ms"], 10)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10)
        self.assertEqual(sum(x["ms"] for x in frame["segments"] if x["tid"] == 2), 8)
        self.assertEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "wake_latency"), 1)

    def test_multiple_frames_and_selection(self):
        events = [event(1, i+1, i*1_000_000, "present", value=500+i) for i in range(15)]
        trace = read(capture(events))
        result = cp.analyze(trace, frames=2)
        self.assertEqual([x["frame"] for x in result["frames"]], [510, 511])
        self.assertEqual(result["average_elapsed_ms"], 1)
        self.assertEqual(len(cp.analyze(trace, 0)["frames"]), 14)

    def test_nested_phases(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "outer"), event(1, 3, 2, "phase_begin", "inner"), event(1, 4, 5, "phase_end", "inner"), event(1, 5, 8, "phase_end", "outer"), event(1, 6, 10, "present", value=2)]
        composition = {x["label"]: x["ms"] for x in self.result(events)["frames"][0]["composition"]}
        self.assertAlmostEqual(composition["outer"], 5e-6)
        self.assertAlmostEqual(composition["outer/inner"], 3e-6)
        self.assertAlmostEqual(composition["UNINSTRUMENTED"], 2e-6)

    def test_unknown_never_matches_object(self):
        events = fixture()
        events[2]["peer"] = 0
        result = self.result(events)
        self.assertEqual(result["unknown_edges"], 1)
        self.assertFalse(any(x["tid"] == 2 for x in result["frames"][0]["segments"]))
        self.assertEqual(sum(x["ms"] for x in result["frames"][0]["segments"] if x["category"] == "unknown_wait"), 7)

    def test_dropped_source_and_sequence_hole(self):
        events = fixture()
        events.pop(6)
        rows = capture(events)
        rows[-2]["dropped"] = 1
        result = cp.analyze(read(rows), 0)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["unknown_edges"], 1)
        self.assertTrue(any("sequence holes" in x for x in result["warnings"]))

    def test_clock_mismatch(self):
        for values in ({"clock": "wall"}, {"clock_shared": False}):
            with self.assertRaisesRegex(ValueError, "clock"):
                read(capture(fixture(), **values))

    def test_timestamp_tie_is_valid(self):
        events = fixture()
        events[6]["ns"] = events[2]["ns"]
        result = self.result(events)
        self.assertAlmostEqual(sum(x["ms"] for x in result["frames"][0]["segments"]), 10)

    def test_source_before_wait_and_frame(self):
        events = fixture()
        events[0]["ns"] = 1_000_000
        events[6]["ns"] = 0
        events[5]["ns"] = 0
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 9)
        self.assertFalse(any(x["tid"] == 2 for x in frame["segments"]))

    def test_source_before_frame_but_after_wait(self):
        events = fixture()
        events[0]["ns"] = 0
        events[1]["ns"] = 0
        events.insert(2, event(3, 1, 9_000_000, "present", value=1000))
        events[4]["value"] = 1001
        result = cp.analyze(read(capture(events)), 2)
        self.assertEqual(result["frames"][0]["elapsed_ms"], 1)
        self.assertAlmostEqual(sum(x["ms"] for x in result["frames"][0]["segments"]), 1)

    def test_future_edge(self):
        events = fixture()
        events[6]["ns"] = 10_000_000
        events[7]["ns"] = 11_000_000
        with self.assertRaisesRegex(ValueError, "future"):
            self.result(events)

    def test_timestamp_regression(self):
        events = fixture()
        events[2]["ns"] = 1
        with self.assertRaisesRegex(ValueError, "regression"):
            self.result(events)

    def test_cycle_on_equal_timestamps(self):
        events = [event(1, 1, 0, "wait_begin"), event(1, 2, 1, "wait_end", value=1 << 48 | 1, peer=2 << 48 | 2), event(2, 1, 0, "wait_begin"), event(2, 2, 1, "wait_end", value=2 << 48 | 1, peer=1 << 48 | 2)]
        with self.assertRaisesRegex(ValueError, "cyclic"):
            read(capture(events))

    def test_invalid_id_and_missing_field(self):
        for field, value in (("id", 1 << 64), ("ns", -1), ("tid", True), ("id", 1 << 48)):
            events = fixture()
            events[0][field] = value
            with self.assertRaises(ValueError):
                self.result(events)
        events = fixture()
        del events[0]["id"]
        with self.assertRaises(ValueError):
            self.result(events)

    def test_duplicate_id(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.result(fixture() + [fixture()[0]])

    def test_missing_footer(self):
        result = cp.analyze(read(capture(fixture())[:-1]), 0)
        self.assertTrue(result["truncated"])

    def test_slack_not_claimed(self):
        frame = self.result()["frames"][0]
        self.assertIn("unprovable", frame["schedule_slack"])
        self.assertTrue(frame["observed_offpath_headroom"])
        self.assertNotIn("slack_ms", frame)

    def test_malformed_wait_pair(self):
        events = fixture()
        events[2]["value"] = events[0]["id"]
        with self.assertRaisesRegex(ValueError, "malformed"):
            self.result(events)

    def test_stats_mismatch_is_truncated(self):
        rows = capture(fixture())
        rows[-2]["emitted"] += 1
        self.assertTrue(cp.analyze(read(rows), 0)["truncated"])

    def test_missing_begin_stays_unknown(self):
        events = fixture()
        events.pop(1)
        result = self.result(events)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["unknown_edges"], 1)
        self.assertTrue(any(x["category"] == "unknown_wait" for x in result["frames"][0]["segments"]))

    def test_large_timestamp_precision(self):
        events = [event(1, 1, (1 << 63), "present", value=1), event(1, 2, (1 << 63) + 123, "present", value=2)]
        self.assertEqual(self.result(events)["frames"][0]["elapsed_ms"], 123 / 1e6)

    def test_nested_unknown_outer_preserves_inner_cause(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 1, "wait_begin", "outer"), event(1, 3, 2, "wait_begin", "inner"), event(1, 4, 8, "wait_end", "inner", value=1 << 48 | 3, peer=2 << 48 | 2), event(1, 5, 9, "wait_end", "outer", value=1 << 48 | 2), event(1, 6, 10, "present", value=2), event(2, 1, 0, "phase_begin", "worker"), event(2, 2, 7, "release")]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["tid"] == 2), 7e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10e-6)

    def test_outer_source_does_not_hide_later_inner_work(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 1, "wait_begin", "outer"), event(1, 3, 3, "wait_begin", "inner"), event(1, 4, 8, "wait_end", "inner", value=1 << 48 | 3, peer=2 << 48 | 3), event(1, 5, 9, "wait_end", "outer", value=1 << 48 | 2, peer=2 << 48 | 2), event(1, 6, 10, "present", value=2), event(2, 1, 0, "phase_begin", "worker"), event(2, 2, 2, "release"), event(2, 3, 7, "release")]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["tid"] == 2), 7e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10e-6)

    def test_marks_label_preceding_interval(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "scope"), event(1, 3, 2, "phase_mark", "first"), event(1, 4, 5, "phase_mark", "second"), event(1, 5, 8, "phase_end", "scope"), event(1, 6, 10, "present", value=2)]
        composition = {x["label"]: x["ms"] for x in self.result(events)["frames"][0]["composition"]}
        self.assertAlmostEqual(composition["scope.first"], 2e-6)
        self.assertAlmostEqual(composition["scope.second"], 3e-6)
        self.assertAlmostEqual(composition["scope"], 3e-6)

    def test_exporter_phase_id_pairing(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "scope"), event(1, 3, 2, "phase_mark", "stage", value=1 << 48 | 2), event(1, 4, 5, "phase_end", "end", value=1 << 48 | 2), event(1, 5, 6, "present", value=2)]
        result = self.result(events)
        self.assertEqual(result["warnings"], [])
        composition = {x["label"]: x["ms"] for x in result["frames"][0]["composition"]}
        self.assertAlmostEqual(composition["scope.stage"], 2e-6)
        self.assertAlmostEqual(composition["scope"], 3e-6)

    def test_exporter_diagnostics_and_name_truncation(self):
        rows = capture(fixture())
        rows[1]["name_truncated"] = True
        rows[-2]["truncated"] = 1
        rows.insert(-1, dict(type="diagnostics", unregistered_events=7))
        result = cp.analyze(read(rows), 0)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["unregistered_events"], 7)
        self.assertEqual(result["name_truncation"], 1)

    def test_partial_even_without_unknown_edges(self):
        result = self.result()
        self.assertEqual(result["unknown_edges"], 0)
        self.assertEqual(result["assessment"], "observed_partial")
        self.assertFalse(result["completeness_proven"])
        self.assertEqual(len(result["missing_constraints"]), 3)

    def test_duplicate_diagnostics_rejected(self):
        rows = capture(fixture())
        rows[-1:-1] = [dict(type="diagnostics", unregistered_events=0)] * 2
        with self.assertRaisesRegex(ValueError, "duplicate diagnostics"):
            read(rows)

    def test_resolved_source_inside_waiter_phase_is_not_lost(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 1, "wait_begin", "outer"), event(1, 3, 2, "phase_begin", "host"), event(1, 4, 8, "phase_end", "host"), event(1, 5, 10, "wait_end", "outer", value=1 << 48 | 2, peer=2 << 48 | 2), event(1, 6, 11, "present", value=2), event(2, 1, 0, "phase_begin", "producer"), event(2, 2, 5, "release")]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["tid"] == 2), 5e-6)
        host = [x for x in frame["segments"] if x["label"] == "host"]
        self.assertEqual([(x["start_ns"], x["end_ns"]) for x in host], [(5, 8)])
        self.assertFalse(any(x["category"] == "unknown_wait" for x in frame["segments"]))
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 11e-6)
        self.assertTrue(frame["causal_ambiguities"])

    def test_pending_outer_source_and_nested_gpu_source(self):
        for nested_ns, chosen_tid in ((6, 3), (4, 2)):
            with self.subTest(nested_ns=nested_ns):
                events = [event(1, 1, 0, "present", value=1), event(1, 2, 1, "wait_begin", "outer"), event(1, 3, 2, "phase_begin", "host"), event(1, 4, 3, "wait_begin", "gpu"), event(1, 5, 7, "wait_end", "gpu", value=1 << 48 | 4, peer=3 << 48 | 2), event(1, 6, 8, "phase_end", "host"), event(1, 7, 10, "wait_end", "outer", value=1 << 48 | 2, peer=2 << 48 | 2), event(1, 8, 11, "present", value=2), event(2, 1, 0, "phase_begin", "producer"), event(2, 2, 5, "release"), event(3, 1, 0, "phase_begin", "gpu_completion"), event(3, 2, nested_ns, "release")]
                frame = self.result(events)["frames"][0]
                self.assertTrue(any(x["tid"] == chosen_tid for x in frame["segments"]))
                self.assertFalse(any(x["tid"] == (2 if chosen_tid == 3 else 3) for x in frame["segments"]))
                self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 11e-6)
                self.assertTrue(frame["causal_ambiguities"])

    def window_rows(self, events=None):
        rows = capture(fixture() if events is None else events, capture_mode="window_only", phase_marks="omitted")
        rows.insert(-1, dict(type="capture_window", opened=True, closed=True, start_ns=0, end_ns=10_000_000, start_present_index=800, end_present_index=801, boundary_unknown=True))
        return rows

    def test_retained_metadata_names_every_report_row(self):
        rows = self.window_rows()
        rows.insert(-1, dict(type="thread_metadata", tid=1, name="presenter", name_truncated=False))
        rows.insert(-1, dict(type="thread_metadata", tid=2, name="queue worker 0", name_truncated=False))
        rows.insert(-1, dict(type="thread_metadata", tid=3, name="startup-only", name_truncated=False))
        trace = read(rows)
        self.assertEqual(trace["names"][3], "startup-only")
        result = cp.analyze(trace)
        self.assertEqual(result["selected_frames"], 1)
        self.assertFalse(result["truncated"])
        for row in result["average_composition"]:
            self.assertEqual(row["thread"], trace["names"][row["tid"]])
        for frame in result["frames"]:
            for key in ("segments", "composition", "observed_offpath_headroom"):
                for row in frame[key]:
                    self.assertEqual(row["thread"], trace["names"][row["tid"]])

    def test_missing_name_is_explicit(self):
        result = self.result()
        self.assertTrue(all(row["thread"] == f"unknown thread (tid {row['tid']})" for row in result["average_composition"]))

    def test_legacy_thread_names(self):
        events = fixture()
        events[5]["kind"] = "thread_name"
        events[5]["name"] = "legacy worker"
        self.assertEqual(read(capture(events))["names"][2], "legacy worker")

    def test_window_boundary_missing_history_is_not_loss(self):
        events = [event(1, 1, 0, "present", value=800), event(1, 2, 2, "wait_begin", object=7), event(1, 3, 9, "wait_end", object=7, value=1 << 48 | 2, peer=2 << 48 | 1), event(1, 4, 10, "present", value=801), event(2, 1, 8, "release", object=7)]
        result = cp.analyze(read(self.window_rows(events)))
        self.assertFalse(result["truncated"])
        self.assertTrue(any(row["label"] == "history before capture window unavailable" for row in result["frames"][0]["segments"]))
        self.assertFalse(result["completeness_proven"])

    def test_window_sequence_hole_is_still_loss(self):
        events = fixture()
        events.pop(5)
        result = cp.analyze(read(self.window_rows(events)))
        self.assertTrue(result["truncated"])
        self.assertTrue(any("sequence holes" in row for row in result["warnings"]))

    def test_window_missing_begin_not_invented(self):
        events = fixture()
        events[1]["kind"] = "point"
        events[2]["value"] = 0
        result = cp.analyze(read(self.window_rows(events)))
        self.assertFalse(result["truncated"])
        self.assertEqual(result["unknown_edges"], 1)
        self.assertTrue(any("boundary or lost" in row["label"] for row in result["frames"][0]["segments"]))

    def test_cap_diagnostics(self):
        rows = self.window_rows()
        rows.insert(-1, dict(type="diagnostics", unregistered_events=0, cap_reached=True, cap_dropped=12))
        rows[-4]["dropped"] = 12
        result = cp.analyze(read(rows))
        self.assertTrue(result["truncated"])
        self.assertEqual(result["cap_dropped"], 12)
        self.assertTrue(any("byte cap" in row for row in result["warnings"]))

    def test_missing_window_metadata(self):
        result = cp.analyze(read(capture(fixture(), capture_mode="window_only")))
        self.assertTrue(result["truncated"])
        self.assertIn("missing capture_window metadata", result["warnings"])

    def test_unclosed_window_is_explicit(self):
        rows = self.window_rows()
        rows[-2].update(closed=False, end_ns=0)
        result = cp.analyze(read(rows))
        self.assertFalse(result["capture_window"]["closed"])
        self.assertIn("requested capture window did not complete", result["warnings"])

    def test_invalid_metadata_and_window(self):
        for row in (dict(type="thread_metadata", tid=1, name=7, name_truncated=False), dict(type="thread_metadata", tid=1, name="worker", name_truncated=1)):
            rows = self.window_rows()
            rows.insert(-1, row)
            with self.assertRaisesRegex(ValueError, "metadata"):
                read(rows)
        rows = self.window_rows()
        rows.insert(-1, rows[-2].copy())
        with self.assertRaisesRegex(ValueError, "duplicate capture"):
            read(rows)

    def test_metadata_truncation(self):
        rows = self.window_rows()
        rows.insert(-1, dict(type="thread_metadata", tid=1, name="part", name_truncated=True))
        self.assertEqual(cp.analyze(read(rows))["thread_name_truncation"], 1)

    def test_gzip_cli_and_truncated_gzip(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2]) as directory:
            path = Path(directory) / "trace.jsonl.gz"
            with gzip.open(path, "wt", encoding="utf-8") as stream:
                stream.write("\n".join(json.dumps(row) for row in self.window_rows()))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(cp.main([str(path), "--json"]), 0)
            self.assertEqual(json.loads(output.getvalue())["selected_frames"], 1)
            path.write_bytes(path.read_bytes()[:-8])
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
                cp.main([str(path)])
            self.assertEqual(failure.exception.code, 2)

    def test_exact_handoff_before_local_poll(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 6, "point", "poll.timeout"), event(1, 3, 8, "handoff", "work.start", peer=2 << 48 | 2), event(1, 4, 10, "present", value=2), event(2, 1, 0, "phase_begin", "producer"), event(2, 2, 3, "release", "work.ready")]
        frame = self.result(events)["frames"][0]
        self.assertEqual(sum(row["ms"] for row in frame["segments"] if row["tid"] == 2), 3e-6)
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 10e-6)
        self.assertTrue(any(row["category"] == "handoff_elapsed" for row in frame["segments"]))
        self.assertFalse(any(row["category"] == "unknown_wait" for row in frame["segments"]))
        self.assertTrue(frame["causal_ambiguities"])

    def test_handoff_before_frame_boundary(self):
        events = [event(1, 1, 5, "present", value=1), event(1, 2, 8, "handoff", "work.start", peer=2 << 48 | 1), event(1, 3, 10, "present", value=2), event(2, 1, 3, "release", "work.ready")]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 5e-6)
        self.assertEqual(frame["segments"][0]["start_ns"], 5)
        self.assertEqual(frame["segments"][0]["category"], "handoff_elapsed")

    def test_missing_and_future_handoff_sources(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 8, "handoff", "work.start"), event(1, 3, 10, "present", value=2)]
        result = self.result(events)
        self.assertEqual(result["unknown_edges"], 1)
        self.assertTrue(any(row["category"] == "unknown" for row in result["frames"][0]["segments"]))
        events[1]["peer"] = 2 << 48 | 1
        events.append(event(2, 1, 9, "release"))
        with self.assertRaisesRegex(ValueError, "future"):
            self.result(events)

    def compact_rows(self):
        rows = self.window_rows()
        rows[0]["event_encoding"] = "array-v1"
        return [[row.get(field, False) for field in cp.EVENT_FIELDS] if row.get("type") == "event" else row for row in rows]

    def test_compact_matches_object_capture(self):
        self.assertEqual(cp.analyze(read(self.compact_rows())), cp.analyze(read(self.window_rows())))

    def test_compact_requires_encoding_header(self):
        rows = self.compact_rows()
        del rows[0]["event_encoding"]
        with self.assertRaisesRegex(ValueError, "array-v1"):
            read(rows)
        with self.assertRaisesRegex(ValueError, "array-v1"):
            read(rows[1:])
        rows = self.compact_rows()
        rows[0]["event_encoding"] = "array-v2"
        with self.assertRaisesRegex(ValueError, "unsupported"):
            read(rows)

    def test_compact_exact_field_count(self):
        for count in (0, 8, 10):
            rows = self.compact_rows()
            rows[1] = (rows[1] + [0])[:count]
            with self.assertRaisesRegex(ValueError, "nine fields"):
                read(rows)

    def test_compact_field_types_validated(self):
        for index, invalid in ((0, True), (1, -1), (2, "0"), (3, []), (4, None), (5, -1), (6, False), (7, "0"), (8, 0)):
            rows = self.compact_rows()
            rows[1][index] = invalid
            with self.subTest(index=index), self.assertRaises(ValueError):
                read(rows)

    def test_compact_after_footer_rejected(self):
        rows = self.compact_rows()
        rows.append(rows[1].copy())
        with self.assertRaisesRegex(ValueError, "after footer"):
            read(rows)

    def label_chain(self):
        return [event(1, 1, 0, "present", value=1), event(1, 2, 2, "wait_begin", "label.landing", object=99), event(1, 3, 12, "handoff", "label.landing.source", object=2 << 48 | 2, peer=3 << 48 | 4), event(1, 4, 13, "wait_end", "label.landing.settled", object=99, value=1 << 48 | 2, peer=1 << 48 | 3), event(1, 5, 14, "present", value=2), event(2, 1, 0, "phase_begin", "producer"), event(2, 2, 5, "release", "label.table.publish", object=99), event(2, 3, 5, "label_batch", object=2 << 48 | 2, value=77), event(3, 1, 0, "point"), event(3, 2, 7, "gpu_submit_begin", object=77, value=8), event(3, 3, 8, "gpu_submit_end", object=77, value=8, peer=3 << 48 | 2), event(3, 4, 11, "gpu_observed", object=77, value=8, peer=3 << 48 | 2)]

    def test_label_exact_record_submit_observation_split(self):
        frame = self.result(self.label_chain())["frames"][0]
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 14e-6)
        self.assertEqual(frame["label_path_split_ms"], dict(waiter_elapsed_before_record=0, producer_path_elapsed=3e-6, submit_delay=2e-6, observed_gpu_roundtrip=4e-6, no_block_host_elapsed=0))
        self.assertTrue(any(row["tid"] == 2 and row["label"] == "producer" for row in frame["segments"]))
        self.assertTrue(any("NOT GPU execution" in row["label"] for row in frame["segments"]))

    def test_label_chain_missing_stale_failed_ambiguous_is_unknown(self):
        for change in ("missing_record", "missing_mapping", "serial", "failed", "mixed"):
            events = self.label_chain()
            if change == "missing_record":
                events[2]["object"] = 99
            elif change == "missing_mapping":
                events[7]["object"] = 99
            elif change == "serial":
                events[-1]["value"] += 1
            elif change == "failed":
                events[-2].update(kind="point", peer=0, name="gpu.submit.failed")
            else:
                events.append(event(4, 1, 6, "label_batch", object=2 << 48 | 2, value=78))
            with self.subTest(change=change):
                frame = self.result(events)["frames"][0]
                self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)
                self.assertTrue(any(row["category"] == "unknown" for row in frame["segments"]))
                self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 14e-6)

    def test_label_already_satisfied_has_no_unknown_dependency(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 2, "wait_begin", "label.wait", object=7), event(1, 3, 3, "wait_end", "label.already_satisfied", object=7, value=1 << 48 | 2), event(1, 4, 4, "present", value=2)]
        report = self.result(events)
        self.assertEqual(report["unknown_edges"], 0)
        self.assertEqual(report["average_label_path_split_ms"]["no_block_host_elapsed"], 1e-6)

    def test_label_chain_capture_boundary(self):
        events = self.label_chain()
        events[0]["ns"] = 6
        events[1]["ns"] = 6
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 8e-6)
        self.assertEqual(frame["label_path_split_ms"]["submit_delay"], 1e-6)
        self.assertEqual(frame["label_path_split_ms"]["producer_path_elapsed"], 0)

    def test_label_nested_wait_keeps_exact_chain_and_phase(self):
        events = self.label_chain()
        events[2:5] = [event(1, 3, 3, "phase_begin", "nested"), event(1, 4, 4, "wait_begin", "gpu.timeline.external", object=88), event(1, 5, 9, "wait_end", "gpu.timeline.observed", object=88, value=1 << 48 | 4), event(1, 6, 10, "phase_end", "nested"), event(1, 7, 12, "handoff", "label.landing.source", object=2 << 48 | 2, peer=3 << 48 | 4), event(1, 8, 13, "wait_end", "label.landing.settled", object=99, value=1 << 48 | 2, peer=1 << 48 | 7), event(1, 9, 14, "present", value=2)]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 14e-6)
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 4e-6)

    def test_inflight_label_wait_reached_via_mutex_release(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 1, "wait_begin", "mutex", object=7), event(1, 3, 8, "wait_end", "mutex", object=7, value=1 << 48 | 2, peer=2 << 48 | 4), event(1, 4, 9, "present", value=2), event(2, 1, 0, "phase_begin", "Driver.LabelWait"), event(2, 2, 2, "wait_begin", "label.wait", object=99), event(2, 3, 3, "release", "mutex.release", object=7), event(2, 4, 7, "release", "mutex.release", object=7), event(2, 5, 12, "wait_end", "label.table", object=99, value=2 << 48 | 2, peer=3 << 48 | 1), event(3, 1, 11, "release", "label.table.publish", object=99)]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 9e-6)
        self.assertAlmostEqual(frame["label_path_split_ms"]["waiter_elapsed_before_record"], 5e-6)
        self.assertFalse(any(row["tid"] == 3 for row in frame["segments"]))
        self.assertTrue(any(row.get("eventual_producer_tid") == 3 and row.get("eventual_producer_traversed") is False for row in frame["segments"]))
        self.assertTrue(any("future edge" in row["label"] for row in frame["segments"]))

    def test_preexisting_exact_record_is_no_block(self):
        events = fixture()
        events[1]["name"] = "label.wait"
        events[2]["name"] = "label.history"
        events[5]["ns"] = 0
        events[6]["ns"] = 1_000_000
        report = self.result(events)
        self.assertEqual(report["average_label_path_split_ms"]["no_block_host_elapsed"], 7)
        self.assertEqual(report["average_label_path_split_ms"]["producer_path_elapsed"], 0)

    def test_deferred_landing_uses_original_record_not_latest_address(self):
        events = self.label_chain()
        events.extend([event(4, 1, 6, "point", "label.landing.deferred", object=2 << 48 | 2, value=2 << 48 | 2), event(4, 2, 9, "release", "label.table.publish", object=99), event(4, 3, 9, "label_batch", object=4 << 48 | 2, value=123)])
        frame = self.result(events)["frames"][0]
        self.assertEqual(frame["label_path_split_ms"]["submit_delay"], 2e-6)
        self.assertFalse(any(row["tid"] == 4 for row in frame["segments"]))
        self.assertTrue(any(row.get("label_producer_event") == 2 << 48 | 2 for row in frame["segments"]))

    def test_gpu_provenance_eviction_is_reported(self):
        events = self.label_chain() + [event(4, 1, 1, "point", "gpu.provenance.evicted")]
        self.assertTrue(any("evicted" in warning for warning in self.result(events)["warnings"]))

    def late_label(self, close_ns, address=99):
        return [event(1, 1, 0, "present", value=1), event(1, 2, 5, "wait_begin", "label.wait", object=address), event(1, 3, 9, "wait_end", "label.table.late.closed", object=address, value=1 << 48 | 2, peer=2 << 48 | 3), event(1, 4, 10, "present", value=2), event(2, 1, 0, "phase_begin", "producer"), event(2, 2, 2, "release", "label.table.publish", object=address), event(2, 3, close_ns, "handoff", "label.table.closed", object=2 << 48 | 2, value=123, peer=2 << 48 | 2)]

    def test_late_cpu_close_before_and_during_wait(self):
        for address in (0x1000, 0x100000000):
            for close_ns in (3, 7):
                with self.subTest(address=address, close_ns=close_ns):
                    frame = self.result(self.late_label(close_ns, address))["frames"][0]
                    self.assertAlmostEqual(sum(row["ms"] for row in frame["segments"]), 10e-6)
                    self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)
                    self.assertEqual(frame["label_path_split_ms"]["submit_delay"], 0)
                    self.assertEqual(frame["label_path_split_ms"]["no_block_host_elapsed"], 4e-6 if close_ns == 3 else 0)
                    self.assertEqual(any(row["tid"] == 2 for row in frame["segments"]), close_ns == 7)

    def test_late_cpu_close_requires_exact_original_record(self):
        for field in ("object", "peer"):
            events = self.late_label(3)
            events[-1][field] = 2 << 48 | 1
            result = self.result(events)
            self.assertGreater(result["unknown_edges"], 0)
            self.assertEqual(result["average_label_path_split_ms"]["no_block_host_elapsed"], 0)
            self.assertEqual(result["average_label_path_split_ms"]["observed_gpu_roundtrip"], 0)

    def test_late_missing_close_and_aborted_remain_unknown(self):
        for name in ("label.table.late.unknown", "label.aborted"):
            events = self.late_label(3)
            events[2].update(name=name, peer=0)
            result = self.result(events)
            self.assertEqual(result["unknown_edges"], 1)
            self.assertEqual(result["average_label_path_split_ms"]["no_block_host_elapsed"], 0)

    def test_gpu_provenance_allocation_failure_is_reported(self):
        events = self.label_chain() + [event(4, 1, 1, "point", "gpu.provenance.unavailable")]
        result = self.result(events)
        self.assertTrue(any("allocation failure" in warning for warning in result["warnings"]))
        self.assertEqual(result["average_label_path_split_ms"], self.result(self.label_chain())["average_label_path_split_ms"])

    def test_gpu_chain_rejects_wrong_event_kind_and_names(self):
        for index, field, value in ((6, "kind", "point"), (6, "name", "mutex.release"), (7, "name", "unrelated.batch"), (9, "name", "unrelated.submit"), (10, "name", "unrelated.success"), (11, "name", "unrelated.observation"), (7, "ns", 4)):
            events = self.label_chain()
            events[index][field] = value
            if index == 7 and field == "ns":
                events[6]["ns"] = 4
                events[7]["tid"] = 4
                events[7]["id"] = 4 << 48 | 1
                events[7]["ns"] = 3
            with self.subTest(index=index, field=field):
                result = self.result(events)
                self.assertEqual(result["average_label_path_split_ms"]["observed_gpu_roundtrip"], 0)
                self.assertGreater(result["unknown_edges"], 0)

    def mutex_policy_fixture(self):
        return [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "waiter.work"), event(1, 3, 2, "wait_begin", "GpuMutex", object=7), event(1, 4, 10, "wait_end", "mutex.release", object=7, value=1 << 48 | 3, peer=2 << 48 | 5), event(1, 5, 12, "present", value=2), event(2, 1, 0, "phase_begin", "holder.LabelWait"), event(2, 2, 1, "point", "mutex.acquire", object=7), event(2, 3, 4, "phase_begin", "Recorder.Submit"), event(2, 4, 7, "phase_end", "Recorder.Submit"), event(2, 5, 9, "release", "mutex.release", object=7)]

    def test_mutex_policy_continues_waiter_not_holder_history(self):
        frame = self.result(self.mutex_policy_fixture())["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 12e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_held"), 7e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_wait_uncovered"), 1e-6)
        self.assertTrue(all(x["start_ns"] >= 2 for x in frame["segments"] if x["tid"] == 2))
        self.assertTrue(any(x["tid"] == 1 and x["start_ns"] == 0 and x["end_ns"] == 2 for x in frame["segments"]))
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x.get("holder_phase") == "holder.LabelWait/Recorder.Submit"), 3e-6)

    def test_mutex_policy_splits_proven_multiple_holders(self):
        events = self.mutex_policy_fixture()
        events[-3]["ns"] = 3
        events[-2]["ns"] = 4
        events[-1]["ns"] = 5
        events[3]["peer"] = 3 << 48 | 3
        events.extend([event(3, 1, 0, "phase_begin", "commit"), event(3, 2, 6, "point", "mutex.acquire", object=7), event(3, 3, 9, "release", "mutex.release", object=7)])
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_held"), 6e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_wait_uncovered"), 2e-6)
        self.assertEqual({x["holder_tid"] for x in frame["segments"] if x["category"] == "mutex_held"}, {2, 3})

    def test_mutex_release_alone_never_fabricates_hold(self):
        events = self.mutex_policy_fixture()
        events[6]["name"] = "no.acquire.evidence"
        frame = self.result(events)["frames"][0]
        self.assertFalse(any(x["category"] == "mutex_held" for x in frame["segments"]))
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_wait_uncovered"), 8e-6)

    def test_mutex_holder_gap_breaks_proof(self):
        events = self.mutex_policy_fixture()
        events.pop(7)
        frame = self.result(events)["frames"][0]
        self.assertFalse(any(x["category"] == "mutex_held" for x in frame["segments"]))

    def test_mutex_legacy_acquired_and_marker_do_not_double_count(self):
        events = self.mutex_policy_fixture()
        events[5:] = [event(2, 1, 0, "wait_begin", "GpuMutex", object=7), event(2, 2, 1, "wait_end", "mutex.release", object=7, value=2 << 48 | 1), event(2, 3, 3, "point", "mutex.acquire", object=7), event(2, 4, 8, "point"), event(2, 5, 9, "release", "mutex.release", object=7)]
        report = self.result(events)
        self.assertEqual(report["proven_mutex_holds"], 1)
        self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["segments"] if x["category"] == "mutex_held"), 7e-6)

    def test_mutex_capture_boundary_clips_hold_and_preserves_path(self):
        events = self.mutex_policy_fixture()
        events[0]["ns"] = 5
        events[1]["ns"] = 5
        events[2]["ns"] = 5
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 7e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "mutex_held"), 4e-6)

    def test_aborted_mutex_wait_never_proves_acquisition(self):
        for outcome in ("unknown", "aborted", "cancelled"):
            events = self.mutex_policy_fixture()
            events[5:] = [event(2, 1, 0, "wait_begin", "GpuMutex", object=7), event(2, 2, 1, "wait_end", outcome, object=7, value=2 << 48 | 1), event(2, 3, 3, "point"), event(2, 4, 8, "point"), event(2, 5, 9, "release", "mutex.release", object=7)]
            with self.subTest(outcome=outcome):
                report = self.result(events)
                self.assertEqual(report["proven_mutex_holds"], 0)
                self.assertFalse(any(x["category"] == "mutex_held" for x in report["frames"][0]["segments"]))

    def test_landing_observation_predates_wait_stays_on_waiter(self):
        events = self.label_chain()
        events[1]["ns"] = 11
        events[-1]["ns"] = 10
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 14e-6)
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)
        self.assertEqual(frame["label_path_split_ms"]["submit_delay"], 0)
        self.assertEqual(frame["label_path_split_ms"]["no_block_host_elapsed"], 2e-6)
        self.assertFalse(any(x["tid"] in {2, 3} for x in frame["segments"]))

    def pipe_fixture(self, enqueue_ns=40):
        return [event(1, 1, 0, "phase_begin", "producer"), event(1, 2, 10, "release", "pipe.enqueue", object=7, value=1), event(1, 3, enqueue_ns, "release", "pipe.enqueue", object=7, value=2), event(2, 1, 0, "present", value=1), event(2, 2, 20, "handoff", "pipe.commit.begin", object=7, value=11, peer=1 << 48 | 2), event(2, 3, 20, "phase_begin", "commit.previous"), event(2, 4, 70, "phase_end", "commit.previous"), event(2, 5, 70, "release", "pipe.retire", object=7, value=0), event(2, 6, 75, "wait_begin", "pipe.committer.idle", object=7), event(2, 7, 85, "wait_end", "item_enqueued", object=7, value=2 << 48 | 6, peer=1 << 48 | 3), event(2, 8, 90, "handoff", "pipe.commit.begin", object=7, value=11, peer=1 << 48 | 3), event(2, 9, 100, "release", "pipe.retire", object=7, value=1), event(2, 10, 110, "present", value=2)]

    def test_pipe_later_predecessor_and_retirement_tie(self):
        for enqueue_ns in (40, 70, 80):
            events = self.pipe_fixture(enqueue_ns)
            trace = read(capture(events))
            expected = 2 << 48 | 5 if enqueue_ns <= 70 else 1 << 48 | 3
            self.assertEqual(trace["pipe_predecessors"][2 << 48 | 8]["id"], expected)
            frame = cp.analyze(trace, 0)["frames"][0]
            self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 110e-6)
            self.assertEqual(any(x["label"] == "commit.previous" for x in frame["segments"]), enqueue_ns <= 70)
            if enqueue_ns <= 70:
                self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "committer_continuation_elapsed"), 20e-6)

    def test_pipe_missing_retire_wrong_identity_and_gap_unknown(self):
        for change in ("missing", "wrong_object", "wrong_serial", "source_object", "gap", "missing_current"):
            events = self.pipe_fixture()
            if change == "missing":
                events.pop(7)
            elif change == "wrong_object":
                events[7]["object"] = 8
            elif change == "wrong_serial":
                events[7]["value"] = 99
            elif change == "source_object":
                events[2]["object"] = 8
            elif change == "gap":
                events.pop(6)
            else:
                events.pop(11)
            with self.subTest(change=change):
                trace = read(capture(events))
                self.assertNotIn(2 << 48 | 8, trace["pipe_predecessors"])
                self.assertIn(2 << 48 | 8, trace["unknown"])
                frame = cp.analyze(trace, 0)["frames"][0]
                self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 110e-6)

    def test_pipe_enqueue_counter_reset_preserves_exact_lifecycle(self):
        events = self.pipe_fixture()
        events[1]["value"] = 700
        events[2]["value"] = 1
        events[7]["value"] = 1000
        events[11]["value"] = 1001
        trace = read(capture(events))
        self.assertEqual(trace["pipe_predecessors"][2 << 48 | 8]["id"], 2 << 48 | 5)
        self.assertAlmostEqual(sum(x["ms"] for x in cp.analyze(trace, 0)["frames"][0]["segments"]), 110e-6)

    def generic_gpu_wait_fixture(self):
        return [event(1, 1, 0, "present", value=1), event(1, 2, 3, "wait_begin", "gpu.timeline.external", object=9), event(1, 3, 9, "wait_end", "gpu.timeline.observed", object=9, value=1 << 48 | 2, peer=3 << 48 | 2), event(1, 4, 10, "present", value=2), event(2, 1, 0, "phase_begin", "submitter"), event(2, 2, 4, "gpu_submit_begin", object=77, value=8), event(2, 3, 5, "gpu_submit_end", object=77, value=8, peer=2 << 48 | 2), event(3, 1, 1, "point", "unrelated.observer.history"), event(3, 2, 8, "gpu_observed", object=77, value=8, peer=2 << 48 | 2)]

    def test_generic_gpu_wait_uses_submit_not_observer_history(self):
        frame = self.result(self.generic_gpu_wait_fixture())["frames"][0]
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 4e-6)
        self.assertEqual(frame["label_path_split_ms"]["submit_delay"], 0)
        self.assertFalse(any(x["tid"] == 3 and x["category"] != "observed_gpu_roundtrip" for x in frame["segments"]))
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10e-6)

    def test_generic_gpu_wait_identity_failure_and_preexisting_observation(self):
        for case in ("batch", "serial", "failed", "already"):
            events = self.generic_gpu_wait_fixture()
            if case == "batch":events[-1]["object"] = 88
            elif case == "serial":events[-1]["value"] = 9
            elif case == "failed":events[6].update(kind="point", peer=0)
            else:events[1]["ns"] = 9
            with self.subTest(case=case):
                frame = self.result(events)["frames"][0]
                self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)
                self.assertFalse(any(x["tid"] == 3 for x in frame["segments"]))
                self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10e-6)

    def test_generic_gpu_fence_nested_wait_and_order_validation(self):
        events = self.generic_gpu_wait_fixture()
        events[1]["name"] = "gpu.fence.external"
        events[2]["name"] = "gpu.fence.observed"
        events[2]["id"] = 1 << 48 | 5
        events[3]["id"] = 1 << 48 | 6
        events.extend([event(1, 3, 4, "wait_begin", "nested", object=10), event(1, 4, 6, "wait_end", "nested.unknown", object=10, value=1 << 48 | 3)])
        frame = self.result(events)["frames"][0]
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 4e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 10e-6)
        events = self.generic_gpu_wait_fixture()
        events[-1]["ns"] = 4
        frame = self.result(events)["frames"][0]
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)
        self.assertFalse(any(x["tid"] == 3 for x in frame["segments"]))
        events = self.generic_gpu_wait_fixture()
        events[-1]["ns"] = 7
        events[1]["ns"] = 8
        frame = self.result(events)["frames"][0]
        self.assertEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "gpu_no_block"), 1e-6)
        self.assertEqual(frame["label_path_split_ms"]["observed_gpu_roundtrip"], 0)

    def pipe_full_fixture(self, passed=False):
        return [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "Driver.Draw"), event(1, 3, 1, "phase_begin", "draw_live_enqueue"), event(1, 4, 2, "phase_begin", "pipe.enqueue"), event(1, 5, 3, "wait_begin", "pipe.full", object=7), event(1, 6, 3, "point", "pipe.full.target", object=1 << 48 | 5, value=3, peer=3 << 48 | 2), event(1, 7, 8, "point", "pipe.full.observed_retire", object=1 << 48 | 5, value=int(passed), peer=2 << 48 | 4), event(1, 8, 9, "wait_end", "item_retired", object=7, value=1 << 48 | 5, peer=0 if passed else 2 << 48 | 4), event(1, 9, 10, "phase_end", "end", value=1 << 48 | 4), event(1, 10, 11, "phase_end", "end", value=1 << 48 | 3), event(1, 11, 12, "phase_mark", "draw_enqueued", value=1 << 48 | 2), event(1, 12, 13, "phase_end", "end", value=1 << 48 | 2), event(1, 13, 14, "present", value=2), event(2, 1, 0, "point"), event(2, 2, 2, "handoff", "pipe.commit.begin", object=7, value=2, peer=3 << 48 | 2), event(2, 3, 2, "phase_begin", "draw_live_commit"), event(2, 4, 7, "release", "pipe.retire", object=7, value=90, peer=3 << 48 | 2), event(3, 1, 0, "point"), event(3, 2, 1, "release", "pipe.enqueue", object=7, value=1), event(3, 3, 1, "point", "pipe.item.kind", object=3 << 48 | 2, value=3)]

    def test_pipe_full_exact_item_identity_and_elapsed_split(self):
        report = self.result(self.pipe_full_fixture())
        wait = report["pipe_waits"][0]
        self.assertEqual(wait["target_kind"], "label")
        self.assertEqual(wait["target_enqueue_event"], 3 << 48 | 2)
        self.assertEqual(wait["target_retirement_event"], 2 << 48 | 4)
        self.assertTrue(wait["exact_wake_source"])
        recorded = report["frames"][0]["recorded_draw_intervals"]
        self.assertAlmostEqual(sum(x["ms"] for x in recorded if x["category"] == "draw_enqueued_blocked_elapsed"), 6e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in recorded if x["category"] == "draw_enqueued_work_elapsed"), 6e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["segments"]), 14e-6)
        self.assertTrue(any(x["category"] == "pipe_full_wake_elapsed" and x["blocking_target_kind"] == "label" for x in report["frames"][0]["segments"]))

    def test_pipe_full_passed_target_keeps_identity_but_not_wake_claim(self):
        report = self.result(self.pipe_full_fixture(True))
        wait = report["pipe_waits"][0]
        self.assertEqual(wait["target_kind"], "label")
        self.assertFalse(wait["exact_wake_source"])
        self.assertEqual(wait["observed_target_retirement_event"], 2 << 48 | 4)
        self.assertEqual(wait["retirements_past_target"], 1)
        self.assertIsNone(wait["wake_source_event"])
        self.assertFalse(any(x["tid"] == 2 for x in report["frames"][0]["segments"]))

    def test_pipe_full_same_kind_wrong_item_never_follows_retirement(self):
        events = self.pipe_full_fixture()
        events.extend([event(3, 4, 2, "release", "pipe.enqueue", object=7, value=1), event(3, 5, 2, "point", "pipe.item.kind", object=3 << 48 | 4, value=3)])
        events[5]["peer"] = 3 << 48 | 4
        report = self.result(events)
        wait = report["pipe_waits"][0]
        self.assertEqual(wait["target_enqueue_event"], 3 << 48 | 4)
        self.assertEqual(wait["target_kind"], "label")
        self.assertFalse(wait["exact_wake_source"])
        self.assertFalse(any(x["tid"] == 2 for x in report["frames"][0]["segments"]))

    def test_pipe_full_target_order_pipeline_kind_and_truncation(self):
        for index, field, value in ((5, "value", 99), (5, "ns", 10), (5, "peer", 0), (5, "name_truncated", True), (18, "object", 8), (19, "value", 1)):
            events = self.pipe_full_fixture()
            events[index][field] = value
            if field == "ns":
                events[5]["ns"] = 3
                events[18]["ns"] = 4
                events[19]["ns"] = 4
                events[14]["ns"] = 4
                events[15]["ns"] = 4
            with self.subTest(index=index, field=field, value=value):
                report = self.result(events)
                self.assertEqual(report["pipe_waits"][0]["identity_status"], "UNKNOWN")
                self.assertFalse(any(x["tid"] == 2 for x in report["frames"][0]["segments"]))

    def test_pipe_full_missing_target_and_retirement_stay_unknown(self):
        for index in (5, 15, 16):
            events = self.pipe_full_fixture()
            events.pop(index)
            with self.subTest(index=index):
                report = self.result(events)
                self.assertFalse(report["pipe_waits"][0]["exact_wake_source"])
                if index != 16:
                    self.assertTrue(report["truncated"])

    def test_pipe_retirement_peer_must_match_its_lifecycle(self):
        events = self.pipe_full_fixture()
        events[16]["peer"] = 3 << 48 | 3
        trace = read(capture(events))
        self.assertNotIn(2 << 48 | 4, trace["pipe_retirements"])
        self.assertFalse(cp.analyze(trace, 0)["pipe_waits"][0]["exact_wake_source"])

    def test_pipe_full_duplicate_targets_are_unknown(self):
        events = self.pipe_full_fixture()
        events[6].update(name="pipe.full.target", peer=3 << 48 | 2, value=3)
        report = self.result(events)
        self.assertFalse(report["pipe_waits"][0]["exact_wake_source"])
        self.assertEqual(report["pipe_waits"][0]["target_kind"], "UNKNOWN")

    def test_metadata_peer_does_not_create_causal_traversal(self):
        report = self.result(self.pipe_full_fixture(True))
        self.assertFalse(any(x["tid"] in {2, 3} for x in report["frames"][0]["segments"]))
        events = self.pipe_full_fixture()
        events[5]["name"] = "unrelated.point"
        with self.assertRaisesRegex(ValueError, "non-causal"):
            self.result(events)

    def test_pipe_full_incomplete_wait_records_observed_block_only(self):
        events = self.pipe_full_fixture()
        events = [x for x in events if x["tid"] != 1 or x["id"] <= 1 << 48 | 7]
        events += [event(4, 1, 10, "present", value=2)]
        report = cp.analyze(read(capture(events)[:-1]), 0)
        wait = report["pipe_waits"][0]
        self.assertIsNone(wait["end_ns"])
        self.assertFalse(wait["exact_wake_source"])
        self.assertEqual(wait["target_kind"], "label")
        self.assertTrue(report["truncated"])
        self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["recorded_draw_intervals"] if x["category"] == "draw_enqueued_blocked_elapsed"), 5e-6)

    def test_pipe_full_sequence_gap_does_not_become_work(self):
        events = self.pipe_full_fixture()
        events.pop(6)
        report = self.result(events)
        self.assertFalse(report["pipe_waits"][0]["exact_wake_source"])
        self.assertTrue(any(x["category"] == "unknown" for x in report["frames"][0]["recorded_draw_intervals"]))
        self.assertFalse(any(x["category"] == "draw_enqueued_work_elapsed" and x["start_ns"] < 9 and x["end_ns"] > 3 for x in report["frames"][0]["recorded_draw_intervals"]))

    def test_live_phase_classes_use_nearest_recognized_scope(self):
        for name in sorted(cp.DRAW_PHASES):
            events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", name), event(1, 3, 2, "phase_begin", "generic.Timer"), event(1, 4, 4, "phase_end", "end", value=1 << 48 | 3), event(1, 5, 6, "phase_end", "end", value=1 << 48 | 2), event(1, 6, 8, "present", value=2)]
            with self.subTest(name=name):
                frame = self.result(events)["frames"][0]
                self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == name), 6e-6)

    def test_live_template_refusal_and_fallback_are_disjoint(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "draw_live_commit"), event(1, 3, 1, "phase_begin", "Driver.DrawCommit"), event(1, 4, 2, "phase_begin", "draw_live_commit_template"), event(1, 5, 3, "phase_begin", "Graphics.DrawReplay"), event(1, 6, 5, "phase_end", "end", value=1 << 48 | 5), event(1, 7, 6, "phase_end", "end", value=1 << 48 | 4), event(1, 8, 7, "phase_begin", "draw_live_commit_fallback"), event(1, 9, 8, "phase_begin", "Graphics.DrawReplay"), event(1, 10, 10, "phase_end", "end", value=1 << 48 | 9), event(1, 11, 11, "phase_end", "end", value=1 << 48 | 8), event(1, 12, 12, "phase_end", "end", value=1 << 48 | 3), event(1, 13, 13, "phase_end", "end", value=1 << 48 | 2), event(1, 14, 14, "present", value=2)]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "draw_live_commit_template"), 4e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "draw_live_commit_fallback"), 4e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "draw_live_commit"), 5e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"]), 14e-6)

    def test_live_gather_nested_and_sibling_select(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 0, "phase_begin", "draw_live_select_q0"), event(1, 3, 2, "phase_begin", "draw_live_gather_q0"), event(1, 4, 5, "phase_end", "end", value=1 << 48 | 3), event(1, 5, 6, "phase_end", "end", value=1 << 48 | 2), event(1, 6, 7, "phase_begin", "draw_live_gather_q0"), event(1, 7, 9, "phase_end", "end", value=1 << 48 | 6), event(1, 8, 10, "present", value=2)]
        frame = self.result(events)["frames"][0]
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "draw_live_gather_q0"), 5e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in frame["segments"] if x["category"] == "draw_live_select_q0"), 3e-6)

    def test_idle_blocking_requires_new_failed_predicate_contract(self):
        for name in ("pipe.committer.idle", "pipe.committer.idle.blocked"):
            events = self.pipe_fixture(80)
            events[8]["name"] = name
            report = self.result(events)
            if name.endswith(".blocked"):
                self.assertEqual(len(report["pipe_waits"]), 1)
                self.assertTrue(report["pipe_waits"][0]["exact_wake_source"])
                self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["recorded_draw_intervals"] if x["category"] == "committer_idle_blocked_elapsed"), 10e-6)
            else:
                self.assertFalse(report["pipe_waits"])
            self.assertEqual(read(capture(events))["pipe_predecessors"][2 << 48 | 8]["id"], 1 << 48 | 3)

    def test_idle_immediate_predicate_has_no_blocked_interval(self):
        events = [event(1, 1, 0, "present", value=1), event(1, 2, 2, "point", "committer.poll"), event(1, 3, 5, "present", value=2)]
        report = self.result(events)
        self.assertFalse(report["pipe_waits"])
        self.assertFalse(report["average_recorded_draw_composition"])

    def test_idle_source_before_failed_predicate_is_unknown(self):
        events = self.pipe_fixture(40)
        events[8]["name"] = "pipe.committer.idle.blocked"
        report = self.result(events)
        self.assertFalse(report["pipe_waits"][0]["exact_wake_source"])
        self.assertIn(2 << 48 | 7, read(capture(events))["unknown"])

    def test_recorded_wait_clips_to_frame_without_double_counting(self):
        events = self.pipe_full_fixture()
        events[0]["ns"] = 0
        events += [event(4, 1, 5, "present", value=100)]
        events[12]["value"] = 101
        report = cp.analyze(read(capture(events)), 2)
        intervals = report["frames"][0]["recorded_draw_intervals"]
        self.assertAlmostEqual(sum(x["ms"] for x in intervals if x["category"] == "draw_enqueued_blocked_elapsed"), 4e-6)
        self.assertTrue(all(x["start_ns"] >= 5 for x in intervals))

    def test_legacy_draw_enqueue_explicit_scope_survives_omitted_marks(self):
        events = self.pipe_full_fixture()
        events[2]["name"] = "draw_enqueue"
        events[10].update(kind="point", name="marks.omitted")
        report = self.result(events)
        self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["recorded_draw_intervals"] if x["category"] == "draw_enqueued_blocked_elapsed"), 6e-6)
        self.assertAlmostEqual(sum(x["ms"] for x in report["frames"][0]["recorded_draw_intervals"] if x["category"] == "draw_enqueued_work_elapsed"), 4e-6)

    def test_non_draw_pipe_enqueue_is_not_draw_work(self):
        events = self.pipe_full_fixture()
        events[2]["name"] = "label.enqueue"
        events[10].update(kind="point", name="marks.omitted")
        report = self.result(events)
        recorded = report["frames"][0]["recorded_draw_intervals"]
        self.assertFalse(any(x["category"].startswith("draw_enqueued") for x in recorded))
        self.assertAlmostEqual(sum(x["ms"] for x in recorded if x["category"] == "pipe_full_blocked_elapsed"), 6e-6)

    def test_duplicate_commit_of_same_enqueue_invalidates_identity(self):
        events = self.pipe_full_fixture()
        events.extend([event(2, 5, 8, "handoff", "pipe.commit.begin", object=7, value=2, peer=3 << 48 | 2), event(2, 6, 9, "release", "pipe.retire", object=7, value=91, peer=3 << 48 | 2)])
        trace = read(capture(events))
        self.assertFalse(trace["pipe_retirements"])
        self.assertFalse(cp.analyze(trace, 0)["pipe_waits"][0]["exact_wake_source"])

    def test_all_pipe_item_kinds_and_reset_counter_have_exact_ids(self):
        for value, name in cp.PIPE_ITEM_KINDS.items():
            events = self.pipe_full_fixture()
            events[5]["value"] = events[19]["value"] = value
            events[18]["value"] = 0
            with self.subTest(kind=name):
                wait = self.result(events)["pipe_waits"][0]
                self.assertEqual(wait["target_kind"], name)
                self.assertEqual(wait["target_enqueue_event"], 3 << 48 | 2)
                self.assertTrue(wait["exact_wake_source"])

    def test_drops_and_byte_cap_remain_incomplete_with_exact_pipe_metadata(self):
        for cap in (False, True):
            rows = capture(self.pipe_full_fixture())
            if cap:
                rows.insert(-1, dict(type="diagnostics", unregistered_events=0, cap_reached=True, cap_dropped=1))
            else:
                rows[-2]["dropped"] = 1
            report = cp.analyze(read(rows), 0)
            self.assertTrue(report["truncated"])
            self.assertFalse(report["completeness_proven"])
            self.assertEqual(report["pipe_waits"][0]["target_kind"], "label")

    def test_mutex_waiter_holder_summary_preserves_two_waiters_and_unknown(self):
        events = self.mutex_policy_fixture()
        events.extend([event(3, 1, 13, "phase_begin", "committer"), event(3, 2, 14, "wait_begin", "GpuMutex", object=7), event(3, 3, 21, "wait_end", "mutex.release", object=7, value=3 << 48 | 2, peer=2 << 48 | 7), event(3, 4, 22, "present", value=3), event(2, 6, 13, "point", "mutex.acquire", object=7), event(2, 7, 20, "release", "mutex.release", object=7)])
        report = self.result(events)
        rows = report["average_mutex_waiter_holder_composition"]
        self.assertEqual({x["waiter_tid"] for x in rows}, {1, 3})
        self.assertEqual({x["holder_tid"] for x in rows if x["category"] == "mutex_held"}, {2})
        self.assertTrue(all(x["holder_thread"] == "UNKNOWN" for x in rows if x["category"] == "mutex_wait_uncovered"))
        self.assertAlmostEqual(sum(x["average_ms"] for x in rows), sum(x["average_ms"] for x in report["average_mutex_holder_composition"]))
        self.assertIn("not all recorded", report["mutex_waiter_holder_interpretation"])

    def test_legacy_pipe_wait_keeps_original_path_categories(self):
        events = self.pipe_full_fixture()
        events[5].update(name="legacy.point", peer=0)
        events[6].update(name="legacy.point", peer=0)
        events[16]["peer"] = 0
        events[19]["name"] = "legacy.point"
        events[2]["name"] = "legacy.enqueue"
        events[3]["name"] = "legacy.pipe"
        report = self.result(events)
        self.assertFalse(report["pipe_waits"][0]["new_contract"])
        self.assertTrue(any(x["category"] == "wake_latency" for x in report["frames"][0]["segments"]))
        self.assertFalse(any(x["category"] in {"draw_enqueued_blocked_elapsed", "draw_enqueued_work_elapsed", "pipe_full_wake_elapsed"} for x in report["frames"][0]["segments"]))

    def test_new_pipe_capture_missing_target_does_not_follow_retirement(self):
        events = self.pipe_full_fixture()
        events.pop(5)
        trace = read(capture(events))
        self.assertIn(1 << 48 | 8, trace["unknown"])
        report = cp.analyze(trace, 0)
        self.assertTrue(report["pipe_waits"][0]["new_contract"])
        self.assertFalse(any(x["tid"] == 2 for x in report["frames"][0]["segments"]))

    def test_missing_wait_end_invalidates_later_blocked_interval(self):
        events = self.pipe_full_fixture()
        events.pop(7)
        report = self.result(events)
        recorded = report["frames"][0]["recorded_draw_intervals"]
        self.assertFalse(any(x["category"] == "draw_enqueued_blocked_elapsed" and x["end_ns"] > 8 for x in recorded))
        self.assertFalse(report["pipe_waits"][0]["sequence_complete"])
        self.assertTrue(any(x["category"] == "unknown" and x["start_ns"] == 8 for x in recorded))

    def test_identity_header_prevents_legacy_fallback_when_metadata_lost(self):
        events = self.pipe_full_fixture()
        events[5].update(name="lost.metadata", peer=0)
        events[6].update(name="lost.metadata", peer=0)
        events[16]["peer"] = 0
        events[19]["name"] = "lost.metadata"
        trace = read(capture(events, edge_contract="redesign-v1"))
        self.assertIn(1 << 48 | 8, trace["unknown"])
        self.assertFalse(trace["pipe_waits"][1 << 48 | 5]["exact_wake_source"])

    def test_idle_gap_and_missing_end_invalidate_blocking_proof(self):
        for missing in ("middle", "end"):
            events = self.pipe_fixture(80)
            events[8]["name"] = "pipe.committer.idle.blocked"
            if missing == "middle":
                events[9]["id"] = 2 << 48 | 20
                events[10]["id"] = 2 << 48 | 21
                events[11]["id"] = 2 << 48 | 22
                events[12]["id"] = 2 << 48 | 23
            else:
                events.pop(9)
            with self.subTest(missing=missing):
                report = self.result(events)
                wait = report["pipe_waits"][0]
                self.assertFalse(wait["sequence_complete"])
                self.assertFalse(wait["exact_wake_source"])
                self.assertFalse(any(x["category"] == "committer_idle_blocked_elapsed" and x["end_ns"] > 75 for x in report["frames"][0]["recorded_draw_intervals"]))
                self.assertFalse(any(x.get("blocking_reason") == "pipe.committer.idle.blocked" and x["end_ns"] > 75 for x in report["frames"][0]["segments"]))

    def test_future_edge_contract_is_not_silently_interpreted(self):
        with self.assertRaisesRegex(ValueError, "unsupported edge contract"):
            read(capture(fixture(), edge_contract="future-v2"))

    def test_pipe_wait_cause_name_and_truncation_must_match_contract(self):
        for kind in ("name", "name_truncated"):
            events = self.pipe_full_fixture()
            events[7][kind] = "unrelated.cause" if kind == "name" else True
            with self.subTest(kind=kind):
                report = self.result(events)
                self.assertFalse(report["pipe_waits"][0]["exact_wake_source"])
                self.assertFalse(any(x["tid"] == 2 for x in report["frames"][0]["segments"]))

    def test_empty_capture(self):
        self.assertEqual(self.result([])["selected_frames"], 0)

    def test_invalid_selection(self):
        trace = read(capture(fixture()))
        for skip, frames in ((-1, None), (0, 0)):
            with self.assertRaises(ValueError):
                cp.analyze(trace, skip, frames)


if __name__ == "__main__":
    unittest.main()
