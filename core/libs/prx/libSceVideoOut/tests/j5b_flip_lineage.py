import argparse
import collections
import json
from pathlib import Path
import unittest


def normalize(row):
    if isinstance(row, list):
        if len(row) != 9:
            raise ValueError("invalid array-v1 event length")
        return dict(zip(("id", "tid", "ns", "kind", "name", "object", "value", "peer", "name_truncated"), row), type="event")
    return row


def verify(rows, require_video=False, require_pipe=False, require_end=False):
    rows = [normalize(row) for row in rows]
    events = {row["id"]: row for row in rows if row.get("type") == "event"}
    counts = collections.Counter()
    names = {}
    for row in rows:
        if row.get("type") in ("thread_name", "thread_metadata"):
            names[row["tid"]] = row["name"]
        if row.get("kind") == "thread_name":
            names[row["tid"]] = row["name"]
    for event in events.values():
        if event.get("kind") != "handoff":
            continue
        name = event.get("name")
        if name not in ("video.flip_dequeue", "pipe.commit.begin"):
            continue
        source = events.get(event.get("peer", 0))
        if source is None:
            raise AssertionError(f"{name}: missing exact source {event.get('peer')}")
        expected = "video.flip_queued" if name == "video.flip_dequeue" else "pipe.enqueue"
        if source.get("kind") != "release" or source.get("name") != expected:
            raise AssertionError(f"{name}: wrong source kind/name")
        if source.get("object") != event.get("object"):
            raise AssertionError(f"{name}: source belongs to another request/pipeline")
        if source["ns"] > event["ns"]:
            raise AssertionError(f"{name}: source is in the future")
        if source["tid"] == event["tid"]:
            raise AssertionError(f"{name}: expected producer/consumer thread transfer")
        counts[name] += 1
        if name == "pipe.commit.begin" and event.get("value") in (12, 13):
            counts["end_submission_handoff"] += 1
        if name == "video.flip_dequeue" and "worker" in names.get(source["tid"], ""):
            counts["queue_worker_to_presenter"] += 1
    if require_video and not counts["video.flip_dequeue"]:
        raise AssertionError("no exact flip dequeue lineage")
    if require_pipe and not counts["pipe.commit.begin"]:
        raise AssertionError("no exact pipeline commit lineage")
    if require_end and not counts["end_submission_handoff"]:
        raise AssertionError("no real SubmitEnd/Completion item handoff")
    counts["poll_timeouts"] = sum(e.get("kind") == "wait_end" and e.get("name") == "presenter_poll_timeout" for e in events.values())
    counts["flip_waits"] = sum(e.get("kind") == "wait_end" and e.get("name") == "flip_queued" for e in events.values())
    counts["missing_flip_wait_sources"] = sum(e.get("kind") == "wait_end" and e.get("name") == "flip_queued" and e.get("peer", 0) not in events for e in events.values())
    return dict(counts)


class LineageTests(unittest.TestCase):
    def fixture(self, name="video.flip_dequeue"):
        return [
            {"type": "event", "id": 1, "tid": 2, "ns": 10, "kind": "release", "name": "video.flip_queued" if name.startswith("video") else "pipe.enqueue", "object": 17},
            {"type": "event", "id": 2, "tid": 3, "ns": 30, "kind": "handoff", "name": name, "object": 17, "peer": 1, "value": 13},
        ]

    def test_compact_exact_request(self):
        rows = [[e.get(k, 0) for k in ("id", "tid", "ns", "kind", "name", "object", "value", "peer", "name_truncated")] for e in self.fixture()]
        self.assertEqual(verify(rows, require_video=True)["video.flip_dequeue"], 1)

    def test_exact_request(self):
        self.assertEqual(verify(self.fixture(), require_video=True)["video.flip_dequeue"], 1)

    def test_timeout_does_not_replace_request_source(self):
        rows = self.fixture()
        rows.insert(1, {"type": "event", "id": 3, "tid": 3, "ns": 20, "kind": "wait_end", "name": "presenter_poll_timeout", "peer": 0})
        result = verify(rows, require_video=True)
        self.assertEqual(result["poll_timeouts"], 1)
        self.assertEqual(result["video.flip_dequeue"], 1)

    def test_matching_end_submission_item(self):
        result = verify(self.fixture("pipe.commit.begin"), require_pipe=True, require_end=True)
        self.assertEqual(result["end_submission_handoff"], 1)

    def test_wrong_request_rejected(self):
        rows = self.fixture()
        rows[0]["object"] = 99
        with self.assertRaisesRegex(AssertionError, "another request"):
            verify(rows)

    def test_missing_source_not_inferred(self):
        rows = self.fixture()
        rows[1]["peer"] = 99
        with self.assertRaisesRegex(AssertionError, "missing exact source"):
            verify(rows)

    def test_future_source_rejected(self):
        rows = self.fixture()
        rows[0]["ns"] = 50
        with self.assertRaisesRegex(AssertionError, "future"):
            verify(rows)

    def test_wrong_source_kind_rejected(self):
        rows = self.fixture()
        rows[0]["name"] = "unrelated"
        with self.assertRaisesRegex(AssertionError, "wrong source"):
            verify(rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("--video", action="store_true")
    parser.add_argument("--pipe", action="store_true")
    parser.add_argument("--end-submission", action="store_true")
    args = parser.parse_args()
    if args.trace is None:
        unittest.main(argv=["j5b_flip_lineage"], verbosity=2)
    else:
        with args.trace.open(encoding="utf-8") as stream:
            rows = [json.loads(line) for line in stream if line.strip()]
        print(json.dumps(verify(rows, args.video, args.pipe, args.end_submission), indent=2))
