import argparse
import bisect
import collections
import gzip
import json
import sys
from pathlib import Path

SCHEMA = "aps5-critical-path/1"
KINDS = {"thread_name", "phase_begin", "phase_end", "phase_mark", "wait_begin", "wait_end", "release", "point", "present", "handoff", "gpu_submit_begin", "gpu_submit_end", "gpu_observed", "label_batch"}
EVENT_FIELDS = ("id", "tid", "ns", "kind", "name", "object", "value", "peer", "name_truncated")
LABEL_READY = {"label.table", "label.history", "label.table.late.closed"}
NO_BLOCK = {"label.already_satisfied", "label.held_at_submit"}
CAUSAL = {"wait_end", "handoff", "gpu_submit_end", "gpu_observed"}
MASK = (1 << 48) - 1


def uint(value, bits, label):
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f"{label} must be uint{bits}")
    return value


def read_trace(lines):
    header = None
    events = {}
    stats = {}
    complete = False
    warnings = []
    diagnostics = None
    truncated_names = 0
    metadata = {}
    window = None
    cap_reached = False
    cap_dropped = 0
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except (ValueError, RecursionError) as error:
            raise ValueError(f"line {number}: invalid JSON: {error}") from error
        if isinstance(row, list):
            if header is None or header.get("event_encoding") != "array-v1":
                raise ValueError(f"line {number}: compact event requires array-v1 header")
            if len(row) != len(EVENT_FIELDS):
                raise ValueError(f"line {number}: compact event requires nine fields")
            row = dict(zip(EVENT_FIELDS, row), type="event")
        if not isinstance(row, dict):
            raise ValueError(f"line {number}: expected object")
        kind = row.get("type")
        if complete:
            raise ValueError("data after footer")
        if header is None:
            if kind != "header" or row.get("format") != SCHEMA:
                raise ValueError(f"expected {SCHEMA} header")
            if row.get("clock") != "steady_ns" or row.get("clock_shared") is not True:
                raise ValueError("requires shared steady_ns clock")
            if not isinstance(row.get("coverage"), list) or not all(isinstance(x, str) for x in row["coverage"]):
                raise ValueError("coverage must be a string list")
            for field in ("ring_capacity", "max_threads"):
                if not uint(row.get(field), 64, field):
                    raise ValueError(f"{field} must be positive")
            if row.get("event_encoding") not in (None, "array-v1"):
                raise ValueError("unsupported event encoding")
            header = row
        elif kind == "event":
            for field in ("id", "ns", "object", "value", "peer"):
                uint(row.get(field), 64, field)
            uint(row.get("tid"), 32, "tid")
            if row["id"] == 0 or row["id"] >> 48 != row["tid"] or row["id"] & MASK == 0:
                raise ValueError("invalid event ID/thread/sequence encoding")
            if not isinstance(row.get("kind"), str) or row["kind"] not in KINDS or not isinstance(row.get("name"), str):
                raise ValueError("invalid event kind/name")
            if row["id"] in events:
                raise ValueError("duplicate event ID")
            if row["peer"] and row["kind"] not in CAUSAL:
                raise ValueError("causal peer on non-causal event")
            if "name_truncated" in row and type(row["name_truncated"]) is not bool:
                raise ValueError("name_truncated must be boolean")
            truncated_names += bool(row.get("name_truncated", False))
            events[row["id"]] = row
        elif kind == "thread_metadata":
            tid = uint(row.get("tid"), 32, "tid")
            if not tid or tid in metadata:
                raise ValueError("invalid or duplicate thread metadata")
            if not isinstance(row.get("name"), str) or type(row.get("name_truncated")) is not bool:
                raise ValueError("invalid thread metadata name")
            metadata[tid] = row
        elif kind == "capture_window":
            if window is not None:
                raise ValueError("duplicate capture window")
            for field in ("opened", "closed", "boundary_unknown"):
                if type(row.get(field)) is not bool:
                    raise ValueError(f"{field} must be boolean")
            for field in ("start_ns", "end_ns", "start_present_index", "end_present_index"):
                uint(row.get(field), 64, field)
            if row["closed"] and (not row["opened"] or row["end_ns"] < row["start_ns"] or row["end_present_index"] < row["start_present_index"]):
                raise ValueError("invalid capture window boundaries")
            window = row
        elif kind == "thread_stats":
            tid = uint(row.get("tid"), 32, "tid")
            if tid in stats:
                raise ValueError("duplicate thread stats")
            for field in ("emitted", "dropped"):
                uint(row.get(field), 64, field)
            if "truncated" in row:
                uint(row["truncated"], 64, "truncated")
            stats[tid] = row
        elif kind == "diagnostics":
            if diagnostics is not None:
                raise ValueError("duplicate diagnostics")
            diagnostics = uint(row.get("unregistered_events"), 64, "unregistered_events")
            if "cap_reached" in row and type(row["cap_reached"]) is not bool:
                raise ValueError("cap_reached must be boolean")
            cap_reached = row.get("cap_reached", False)
            cap_dropped = uint(row.get("cap_dropped", 0), 64, "cap_dropped")
        elif kind == "footer":
            if row.get("complete") is not True:
                raise ValueError("invalid footer")
            complete = True
        else:
            raise ValueError(f"unknown row type: {kind}")
    if header is None:
        raise ValueError("missing header")
    if not complete:
        warnings.append("missing footer: capture truncated")
    threads = collections.defaultdict(list)
    for event in events.values():
        threads[event["tid"]].append(event)
    if len(set(threads) | set(metadata) | set(stats)) > header["max_threads"]:
        raise ValueError("thread count exceeds max_threads")
    previous = {}
    phases = {}
    wait_labels = {}
    interval_phases = {}
    names = {}
    windowed = header.get("capture_mode") == "window_only"
    loss = not complete or bool(diagnostics) or cap_reached or bool(cap_dropped)
    if windowed:
        warnings.append("startup history intentionally omitted; boundary phases and causal sources may be unavailable")
        if window is None:
            loss = True
            warnings.append("missing capture_window metadata")
        elif not window["opened"] or not window["closed"]:
            warnings.append("requested capture window did not complete")
    if cap_reached or cap_dropped:
        warnings.append(f"event byte cap reached: {cap_dropped} events rejected; capture incomplete")
    if diagnostics:
        warnings.append(f"unregistered events: {diagnostics} (thread cap or allocation loss)")
    name_loss = max(truncated_names, sum(row.get("truncated", 0) for row in stats.values()))
    if name_loss:
        warnings.append(f"truncated event names: {name_loss}; phase/cause coverage incomplete")
    gaps = set()
    resolved_begins = set()
    for tid, timeline in threads.items():
        timeline.sort(key=lambda x: x["id"])
        stack = []
        waits = []
        prior = None
        for event in timeline:
            eid = event["id"]
            if prior is not None and event["ns"] < prior["ns"]:
                raise ValueError("timestamp regression in thread")
            expected = (prior["id"] & MASK) + 1 if prior else 1
            if eid & MASK != expected:
                gaps.add(eid)
                stack = []
            previous[eid] = prior["id"] if prior else None
            kind = event["kind"]
            if kind == "thread_name":
                names[tid] = event["name"]
            elif kind == "phase_begin":
                stack.append((eid, event["name"]))
            elif kind == "phase_end":
                if not stack or (stack[-1][0] != event["value"] if event["value"] else stack[-1][1] != event["name"]):
                    warnings.append(f"unmatched phase end {eid}: phase coverage incomplete")
                    stack = []
                else:
                    stack.pop()
            if kind == "wait_begin":
                waits.append((eid, event["name"]))
            elif kind == "wait_end":
                waits = [item for item in waits if item[0] != event["value"]]
            wait_labels[eid] = waits[-1] if waits else None
            phases[eid] = "/".join(item[1] for item in stack) or "UNINSTRUMENTED"
            prior = event
        scopes = []
        for event in timeline:
            eid = event["id"]
            if eid in gaps:
                scopes = []
            if scopes:
                interval_phases[eid] = scopes[-1]["name"]
                scopes[-1]["pending"].append(eid)
            if event["kind"] == "phase_begin":
                scopes.append(dict(id=eid, name=phases[eid], pending=[]))
            elif event["kind"] == "phase_mark" and scopes:
                scope = next((item for item in reversed(scopes) if not event["value"] or item["id"] == event["value"]), None)
                if scope:
                    for interval in scope["pending"]:
                        interval_phases[interval] = scope["name"] + "." + event["name"]
                    scope["pending"].clear()
                else:
                    warnings.append(f"unmatched phase mark {eid}: phase coverage incomplete")
            elif event["kind"] == "phase_end" and scopes:
                scopes.pop()
        if stack:
            warnings.append(f"thread {tid}: open phase at capture end")
        info = stats.get(tid)
        if info is None:
            loss = True
            warnings.append(f"thread {tid}: missing thread_stats")
        elif info["emitted"] != len(timeline):
            loss = True
            warnings.append(f"thread {tid}: emitted count differs from retained events")
    for tid, info in stats.items():
        if tid not in threads and info["emitted"]:
            loss = True
            warnings.append(f"thread {tid}: emitted count differs from retained events")
    names.update({tid: row["name"] for tid, row in metadata.items()})
    metadata_name_loss = sum(row["name_truncated"] for row in metadata.values())
    if metadata_name_loss:
        warnings.append(f"truncated thread names: {metadata_name_loss}")
    if gaps:
        warnings.append(f"sequence holes: {len(gaps)} (overflow or missing events)")
    dropped = sum(x["dropped"] for x in stats.values())
    if dropped:
        warnings.append(f"dropped events: {dropped}")
    if any(event["kind"] == "point" and event["name"] == "gpu.provenance.evicted" for event in events.values()):
        warnings.append("bounded GPU provenance metadata evicted; affected label chains remain UNKNOWN")
    if any(event["kind"] == "point" and event["name"] == "gpu.provenance.unavailable" for event in events.values()):
        warnings.append("GPU provenance metadata unavailable after allocation failure; affected label chains remain UNKNOWN")
    wait_ends = {event["value"]: event for event in events.values() if event["kind"] == "wait_end"}
    dependencies = {eid: [p] if (p := previous[eid]) else [] for eid in events}
    unknown = {}
    paired = set()
    for eid, event in events.items():
        if event["kind"] not in CAUSAL:
            continue
        begin = events.get(event["value"])
        source = events.get(event["peer"])
        if event["kind"] != "wait_end":
            pass
        elif begin is None:
            unknown[eid] = "missing wait_begin (not retained at boundary or lost)" if windowed else "missing wait_begin"
        elif begin["kind"] != "wait_begin" or begin["tid"] != event["tid"] or begin["object"] != event["object"] or begin["id"] >= eid or begin["ns"] > event["ns"]:
            raise ValueError("malformed wait_begin reference")
        elif begin["id"] in paired:
            raise ValueError("wait_begin paired more than once")
        else:
            paired.add(begin["id"])
        if event["peer"] == 0 and event["kind"] == "wait_end" and event["name"] in NO_BLOCK:
            continue
        if event["peer"] == 0:
            unknown[eid] = (unknown[eid] + "; " if eid in unknown else "") + "UNKNOWN causal source"
        elif source is None:
            unknown[eid] = (unknown[eid] + "; " if eid in unknown else "") + "missing causal source (dropped or outside capture)"
        else:
            if source["ns"] > event["ns"] or source["id"] == eid or (source["tid"] == event["tid"] and source["id"] >= eid):
                raise ValueError("future/self causal edge")
            dependencies[eid].append(source["id"])
    batches = collections.defaultdict(list)
    submissions = collections.defaultdict(list)
    label_chains = {}
    for event in events.values():
        if event["kind"] == "label_batch":
            batches[event["object"]].append(event)
        elif event["kind"] == "gpu_submit_end":
            submissions[event["peer"]].append(event)
    gpu_chains = {}
    for eid, event in events.items():
        if event["kind"] != "gpu_observed":
            continue
        submit = events.get(event["peer"])
        ends = submissions.get(event["peer"], [])
        if submit and submit["kind"] == "gpu_submit_begin" and submit["name"] == "gpu.host.submit.begin" and event["name"] == "gpu.completion.cpu_observed" and submit["object"] and submit["value"] and event["object"] == submit["object"] and event["value"] == submit["value"] and len(ends) == 1 and ends[0]["name"] == "gpu.host.submit.end" and ends[0]["object"] == submit["object"] and ends[0]["value"] == submit["value"] and submit["ns"] <= ends[0]["ns"] <= event["ns"]:
            gpu_chains[eid] = submit
        else:
            unknown[eid] = "exact successful GPU submission identity/order unavailable"
    for eid, event in events.items():
        source = events.get(event["peer"])
        if event["kind"] == "wait_end" and source and source["kind"] == "gpu_observed" and source["id"] not in gpu_chains:
            unknown[eid] = "GPU wait has no validated successful submission chain"
    for eid, event in events.items():
        if event["kind"] != "handoff" or event["name"] != "label.landing.source":
            continue
        record = events.get(event["object"])
        observed = events.get(event["peer"])
        submit = events.get(observed["peer"]) if observed else None
        mapping = batches.get(event["object"], [])
        ends = submissions.get(submit["id"], []) if submit else []
        reason = None
        if record is None:
            reason = "label record unavailable (outside capture or metadata loss)"
        elif record["kind"] != "release" or record["name"] != "label.table.publish":
            reason = "invalid original label record kind/name"
        elif not observed or observed["kind"] != "gpu_observed" or observed["name"] != "gpu.completion.cpu_observed" or not submit or submit["kind"] != "gpu_submit_begin" or submit["name"] != "gpu.host.submit.begin":
            reason = "exact GPU submission/observation unavailable"
        elif len(mapping) != 1 or mapping[0]["name"] != "label.recorded.batch" or mapping[0]["value"] != submit["object"] or not submit["object"]:
            reason = "label batch mapping unavailable or ambiguous (overflow/mixed writer)"
        elif observed["object"] != submit["object"] or observed["value"] != submit["value"] or not submit["value"]:
            reason = "GPU batch/serial mismatch (stale recorder identity)"
        elif len(ends) != 1 or ends[0]["name"] != "gpu.host.submit.end" or ends[0]["object"] != submit["object"] or ends[0]["value"] != submit["value"]:
            reason = "successful exact submission unavailable (failed/outside capture)"
        elif not record["ns"] <= submit["ns"] <= ends[0]["ns"] <= observed["ns"] <= event["ns"] or not record["ns"] <= mapping[0]["ns"] <= submit["ns"]:
            reason = "invalid label recording/submission/observation order"
        if reason:
            unknown[eid] = reason
        else:
            label_chains[eid] = (record, submit, observed)
            dependencies[eid].append(record["id"])
    for eid, event in events.items():
        if event["kind"] == "wait_end" and event["name"] == "label.table.late.closed":
            close = events.get(event["peer"])
            record = events.get(close["peer"]) if close else None
            if not close or close["kind"] != "handoff" or close["name"] != "label.table.closed" or close["object"] != close["peer"] or not record or record["kind"] != "release" or record["name"] != "label.table.publish":
                unknown[eid] = "exact CPU label group close/original record unavailable"
    pipe_predecessors = {}
    for tid, timeline in threads.items():
        open_items = {}
        retired_items = {}
        for event in timeline:
            eid, obj = event["id"], event["object"]
            if eid in gaps:
                open_items.clear()
                retired_items.clear()
            if event["kind"] == "handoff" and event["name"] == "pipe.commit.begin":
                source = events.get(event["peer"])
                valid = source is not None and source["kind"] == "release" and source["name"] == "pipe.enqueue" and source["object"] == obj
                unknown[eid] = "pipe item lifecycle/previous retirement unavailable"
                if obj in open_items:
                    prior_begin = open_items[obj][0]
                    unknown[prior_begin["id"]] = "nested or duplicate pipe commit begin"
                    unknown[eid] = "nested or duplicate pipe commit begin"
                    valid = False
                    retired_items.pop(obj, None)
                open_items[obj] = (event, source, retired_items.get(obj), valid)
            elif event["kind"] == "release" and event["name"] == "pipe.retire":
                opened = open_items.pop(obj, None)
                if opened is None:
                    retired_items.pop(obj, None)
                    continue
                begin, source, previous_item, valid = opened
                if not valid:
                    unknown[begin["id"]] = "exact pipe enqueue/lifecycle identity unavailable or wrong pipeline"
                    retired_items.pop(obj, None)
                    continue
                if previous_item is None:
                    unknown[begin["id"]] = "previous complete pipe item/retirement unavailable"
                else:
                    previous_begin, previous_retire = previous_item
                    if event["value"] != previous_retire["value"] + 1:
                        unknown[begin["id"]] = "nonconsecutive pipe retirement ordinals"
                    else:
                        chosen = previous_retire if previous_retire["ns"] >= source["ns"] else source
                        pipe_predecessors[begin["id"]] = chosen
                        dependencies[begin["id"]].append(chosen["id"])
                        unknown.pop(begin["id"], None)
                retired_items[obj] = (begin, event)
    indegree = {eid: len(set(edges)) for eid, edges in dependencies.items()}
    outgoing = collections.defaultdict(list)
    for eid, edges in dependencies.items():
        for dep in set(edges):
            outgoing[dep].append(eid)
    ready = collections.deque(eid for eid, count in indegree.items() if not count)
    visited = 0
    while ready:
        visited += 1
        for eid in outgoing[ready.popleft()]:
            indegree[eid] -= 1
            if not indegree[eid]:
                ready.append(eid)
    if visited != len(events):
        raise ValueError("cyclic causal edges")
    resolved_begins = {event["value"] for eid, event in events.items() if event["kind"] == "wait_end" and eid not in unknown}
    thread_gap_ids = {tid: sorted(event["id"] for event in timeline if event["id"] in gaps) for tid, timeline in threads.items()}
    mutex_markers = collections.defaultdict(list)
    for event in events.values():
        if event["kind"] == "release" and event["name"] == "mutex.release":
            mutex_markers[event["object"]].append((event, False))
        elif event["kind"] == "point" and event["name"] == "mutex.acquire":
            mutex_markers[event["object"]].append((event, True))
        elif event["kind"] == "wait_end":
            begin = events.get(event["value"])
            if begin and begin["kind"] == "wait_begin" and begin["name"] == "GpuMutex" and event["name"] == "mutex.release" and begin["object"] == event["object"] and begin["tid"] == event["tid"]:
                mutex_markers[event["object"]].append((event, True))
    mutex_holds = collections.defaultdict(list)
    for obj, markers in mutex_markers.items():
        active = None
        for event, acquired in sorted(markers, key=lambda item: (item[0]["ns"], item[1], item[0]["id"])):
            if acquired:
                if active is None:
                    active = event
                elif not (active["tid"] == event["tid"] and active["kind"] == "wait_end" and event["kind"] == "point"):
                    active = None
            else:
                if active and active["tid"] == event["tid"] and active["ns"] < event["ns"] and not loss and not dropped:
                    holes = thread_gap_ids[event["tid"]]
                    if bisect.bisect_right(holes, active["id"]) == bisect.bisect_right(holes, event["id"]):
                        mutex_holds[obj].append(dict(start_ns=active["ns"], end_ns=event["ns"], tid=event["tid"], acquire_event=active["id"], release_event=event["id"]))
                active = None
    mutex_hold_ends = {obj: [hold["end_ns"] for hold in holds] for obj, holds in mutex_holds.items()}
    thread_times = {tid: [event["ns"] for event in timeline] for tid, timeline in threads.items()}
    presents = sorted((x for x in events.values() if x["kind"] == "present"), key=lambda x: (x["ns"], x["value"]))
    if any(b["value"] <= a["value"] for a, b in zip(presents, presents[1:])):
        raise ValueError("present frame IDs must increase")
    return dict(gpu_chains=gpu_chains, pipe_predecessors=pipe_predecessors, mutex_holds=mutex_holds, mutex_hold_ends=mutex_hold_ends, thread_times=thread_times, wait_ends=wait_ends, label_chains=label_chains, window=window, windowed=windowed, cap_reached=cap_reached, cap_dropped=cap_dropped, metadata_name_loss=metadata_name_loss, resolved_begins=resolved_begins, header=header, events=events, threads=threads, previous=previous, phases=phases, wait_labels=wait_labels, interval_phases=interval_phases, names=names, gaps=gaps, unknown=unknown, warnings=warnings, dropped=dropped, presents=presents, complete=complete, loss=loss, name_loss=name_loss, unregistered_events=diagnostics or 0)


def thread_name(trace, tid):
    return trace["names"].get(tid) or f"unknown thread (tid {tid})"


def walk_frame(trace, start, present):
    events = trace["events"]
    segments = []
    path = set()
    cursor = present["id"]
    stop = present["ns"]
    pending = None
    selected_record = None
    active_gpu_observations = set()
    no_block_bridges = set()
    unresolved_pipe_idle = set()
    producer_ranges = []
    ambiguities = []
    resolved_begins = trace["resolved_begins"]

    def add(left, right, tid, category, label):
        left = max(left, start)
        right = min(right, stop)
        if right > left:
            segments.append(dict(start_ns=left, end_ns=right, tid=tid, thread=thread_name(trace, tid), category=category, label=label, ms=(right-left)/1e6))

    def step(left, right, tid, category, label, target, causal=False):
        nonlocal pending
        if pending is not None and left <= pending["ns"]:
            left = pending["ns"]
            target = pending["id"]
            pending = None
        elif causal and pending is not None:
            ambiguities.append("Later nested causal source selected over earlier outer source by observed timestamp; full max-plus dependency dominance is not proven.")
            pending = None
        add(left, right, tid, category, label)
        return target

    def mutex_wait(begin, event):
        nonlocal pending
        left = max(start, begin["ns"])
        right = event["ns"]
        target = begin["id"]
        if pending is not None and left <= pending["ns"] <= right:
            left = pending["ns"]
            target = pending["id"]
            pending = None
        pieces = []
        covered = left
        holds = trace["mutex_holds"].get(event["object"], [])
        index = bisect.bisect_right(trace["mutex_hold_ends"].get(event["object"], []), left)
        for hold in holds[index:]:
            if hold["start_ns"] >= right:
                break
            a, b = max(left, hold["start_ns"]), min(right, hold["end_ns"])
            if a > covered:
                pieces.append((covered, a, None, "UNKNOWN holder/hold interval: uncontended acquire, missing history, or wake/scheduling delay"))
            timeline = trace["threads"][hold["tid"]]
            pos = bisect.bisect_right(trace["thread_times"][hold["tid"]], a)
            while a < b:
                endpoint = timeline[pos] if pos < len(timeline) else None
                bound = min(b, endpoint["ns"]) if endpoint else b
                phase = trace["interval_phases"].get(endpoint["id"], "UNINSTRUMENTED") if endpoint else "UNINSTRUMENTED"
                if bound > a:
                    pieces.append((a, bound, hold, phase))
                a = bound
                pos += 1
            covered = max(covered, b)
        if covered < right:
            pieces.append((covered, right, None, "UNKNOWN holder/hold interval: uncontended acquire, missing history, or wake/scheduling delay"))
        for a, b, hold, phase in reversed(pieces):
            count = len(segments)
            add(a, b, hold["tid"] if hold else event["tid"], "mutex_held" if hold else "mutex_wait_uncovered", phase)
            if len(segments) > count:
                segments[-1].update(waiter_tid=event["tid"], waiter_thread=thread_name(trace, event["tid"]), mutex_object=event["object"], mutex_wait_begin=begin["id"], mutex_wait_end=event["id"])
                if hold:
                    segments[-1].update(holder_tid=hold["tid"], holder_thread=thread_name(trace, hold["tid"]), holder_phase=phase, holder_acquire_event=hold["acquire_event"], holder_release_event=hold["release_event"])
        return target

    while cursor is not None:
        event = events[cursor]
        path.add(cursor)
        if event["ns"] <= start:
            break
        prior_id = trace["previous"][cursor]
        prior = events.get(prior_id)
        if cursor in trace["pipe_predecessors"]:
            source = trace["pipe_predecessors"][cursor]
            is_retire = source["name"] == "pipe.retire"
            category = "committer_continuation_elapsed" if is_retire else "handoff_elapsed"
            label = "pipe.commit.begin: previous item retired later than or at enqueue" if is_retire else "pipe.commit.begin: enqueue later than previous retirement"
            cursor = step(source["ns"], event["ns"], event["tid"], category, label, source["id"], causal=True)
        elif cursor in no_block_bridges:
            cursor = step(prior["ns"] if prior else start, event["ns"], event["tid"], "label_no_block", "label landing observation predates wait; host check elapsed, not GPU blocking", prior_id)
        elif cursor in trace["label_chains"]:
            record, submit, observed = trace["label_chains"][cursor]
            target = step(observed["ns"], event["ns"], event["tid"], "label_observation_delay", event["name"], observed["id"], causal=True)
            selected_record = (record, submit, observed) if target == observed["id"] else None
            cursor = target
        elif selected_record and cursor == selected_record[2]["id"]:
            record, submit, observed = selected_record
            cursor = step(submit["ns"], observed["ns"], observed["tid"], "observed_gpu_roundtrip", "CPU submit begin to successful CPU observation; includes submission, queueing, driver and host observation delay; NOT GPU execution", submit["id"], causal=True)
            if cursor != submit["id"]:
                selected_record = None
        elif selected_record and cursor == selected_record[1]["id"]:
            record, submit, observed = selected_record
            cursor = step(record["ns"], submit["ns"], submit["tid"], "label_submit_delay", "exact label record to CPU submit begin (elapsed, not CPU execution)", record["id"], causal=True)
            selected_record = None
        elif cursor in active_gpu_observations and cursor in trace["gpu_chains"]:
            submit = trace["gpu_chains"][cursor]
            cursor = step(submit["ns"], event["ns"], event["tid"], "observed_gpu_roundtrip", "CPU submit begin to successful CPU observation; includes submission, queueing, driver and host observation delay; NOT GPU execution", submit["id"], causal=True)
        elif event["kind"] == "handoff":
            source = events.get(event["peer"])
            if cursor in trace["unknown"] or source is None:
                if event["name"] == "pipe.commit.begin" and prior and prior["kind"] == "wait_end" and prior["object"] == event["object"] and prior["peer"] == event["peer"]:
                    idle = events.get(prior["value"])
                    if idle and idle["name"] == "pipe.committer.idle":
                        unresolved_pipe_idle.add(prior["id"])
                left = prior["ns"] if prior else start
                cursor = step(left, event["ns"], event["tid"], "unknown", event["name"] + ": " + trace["unknown"].get(cursor, "missing handoff source"), prior_id)
            else:
                ambiguities.append("Exact work handoff follows its recorded producer; transfer elapsed time is not blocking or CPU work, and max-plus dominance over local/resource dependencies is unproven.")
                cursor = step(source["ns"], event["ns"], event["tid"], "handoff_elapsed", event["name"], source["id"], causal=True)
        elif event["kind"] == "wait_end":
            begin = events.get(event["value"])
            source = events.get(event["peer"])
            if begin is not None and source is not None and cursor not in trace["unknown"]:
                if source["id"] in trace["gpu_chains"] and source["ns"] >= begin["ns"]:
                    active_gpu_observations.add(source["id"])
                if source["id"] in trace["label_chains"]:
                    record = trace["label_chains"][source["id"]][0]
                    producer_ranges.append((begin["ns"], record["ns"], record["id"]))
                elif event["name"] in LABEL_READY:
                    producer_ranges.append((begin["ns"], source["ns"], source["id"]))
            if cursor in unresolved_pipe_idle and begin is not None:
                cursor = step(begin["ns"], event["ns"], event["tid"], "unknown_wait", "pipe predecessor unavailable; idle edge cannot substitute for missing retirement", begin["id"])
            elif begin is not None and source is not None and source["id"] in trace["label_chains"] and trace["label_chains"][source["id"]][2]["ns"] < begin["ns"]:
                no_block_bridges.add(source["id"])
                cursor = step(prior["ns"] if prior else begin["ns"], event["ns"], event["tid"], "label_no_block", "label landing observation predates wait; host check elapsed, not GPU blocking", prior_id)
            elif begin is not None and begin["name"] == "GpuMutex" and event["name"] == "mutex.release":
                cursor = mutex_wait(begin, event)
            elif begin is not None and event["name"] in NO_BLOCK and not event["peer"]:
                cursor = step(prior["ns"] if prior else begin["ns"], event["ns"], event["tid"], "label_no_block", event["name"] + " (check/host elapsed; no blocking dependency)", prior_id)
            elif cursor in trace["unknown"] or begin is None:
                left = prior["ns"] if prior else start
                cursor = step(left, event["ns"], event["tid"], "unknown_wait", event["name"] + ": " + trace["unknown"].get(cursor, "missing wait_begin"), prior_id)
            elif source["id"] in trace["gpu_chains"] and source["ns"] < begin["ns"]:
                cursor = step(prior["ns"] if prior else begin["ns"], event["ns"], event["tid"], "gpu_no_block", "successful GPU observation predates wait; host check elapsed, not GPU blocking", prior_id)
            elif source["ns"] < begin["ns"] and event["name"] in LABEL_READY:
                cursor = step(prior["ns"] if prior else begin["ns"], event["ns"], event["tid"], "label_no_block", event["name"] + " (exact record predates wait; check/host elapsed)", prior_id)
            elif prior and prior["id"] != begin["id"] and prior["ns"] >= source["ns"]:
                if source["ns"] >= begin["ns"]:
                    if pending is None or source["ns"] > pending["ns"]:
                        pending = source
                    ambiguities.append("Waiter events overlap a resolved source; retain observed post-source elapsed time and cut to that exact source. Host work versus blocking and max-plus dominance are unproven.")
                cursor = step(prior["ns"], event["ns"], event["tid"], "wake_latency", event["name"], prior_id)
            elif source["ns"] >= begin["ns"]:
                cursor = step(source["ns"], event["ns"], event["tid"], "wake_latency", event["name"], source["id"], causal=True)
            else:
                cursor = step(begin["ns"], event["ns"], event["tid"], "wake_latency", event["name"] + " (source predates wait)", begin["id"])
        else:
            eventual_source = None
            left = prior["ns"] if prior else start
            phase = trace["interval_phases"].get(cursor, "UNINSTRUMENTED")
            category = "phase_elapsed" if phase != "UNINSTRUMENTED" else "uninstrumented"
            wait = trace["wait_labels"].get(prior_id)
            if cursor in trace["gaps"] or prior is None:
                category = "unknown"
                phase = "history before capture window unavailable" if prior is None and trace["windowed"] and cursor not in trace["gaps"] else "missing thread history"
            elif wait is not None:
                end = trace["wait_ends"].get(wait[0])
                source = events.get(end["peer"]) if end else None
                if source and source["ns"] > event["ns"] and end["name"] in LABEL_READY:
                    eventual_source = source
                    category, phase = "label_wait_elapsed_before_record", wait[1] + ": exact eventual producer is later than this path endpoint; cannot follow a future edge; host work/blocking split unknown"
                elif wait[0] not in resolved_begins:
                    category, phase = "unknown_wait", wait[1] + ": unresolved wait interval"
            segment_count = len(segments)
            cursor = step(left, event["ns"], event["tid"], category, phase, prior_id)
            if eventual_source and len(segments) > segment_count:
                segments[-1].update(eventual_producer_event=eventual_source["id"], eventual_producer_tid=eventual_source["tid"], eventual_producer_thread=thread_name(trace, eventual_source["tid"]), eventual_producer_ns=eventual_source["ns"], eventual_producer_traversed=False)
    segments.reverse()
    producer_ranges = [item for item in producer_ranges if item[2] in path]
    split_segments = []
    for segment in segments:
        cuts = sorted({segment["start_ns"], segment["end_ns"]} | {bound for left, right, record in producer_ranges for bound in (left, right) if segment["start_ns"] < bound < segment["end_ns"]})
        for left, right in zip(cuts, cuts[1:]):
            part = dict(segment, start_ns=left, end_ns=right, ms=(right-left)/1e6)
            matches = [record for begin, end, record in producer_ranges if begin <= left and right <= end]
            if matches and part["category"] not in {"label_submit_delay", "observed_gpu_roundtrip", "label_wait_elapsed_before_record", "label_no_block"}:
                part["label_producer_event"] = matches[-1]
            split_segments.append(part)
    segments = split_segments
    composition = collections.defaultdict(float)
    for segment in segments:
        composition[(segment["tid"], segment["category"], segment["label"])] += segment["ms"]
    headroom = []
    for tid, timeline in trace["threads"].items():
        offpath = [x for x in timeline if start <= x["ns"] <= stop and x["id"] not in path]
        if offpath:
            last = offpath[-1]
            headroom.append(dict(tid=tid, thread=thread_name(trace, tid), last_offpath_event=last["id"], observed_headroom_ms=(stop-last["ns"])/1e6))
    return dict(label_path_split_ms=dict(waiter_elapsed_before_record=sum(x["ms"] for x in segments if x["category"] == "label_wait_elapsed_before_record"), producer_path_elapsed=sum(x["ms"] for x in segments if "label_producer_event" in x), submit_delay=sum(x["ms"] for x in segments if x["category"] == "label_submit_delay"), observed_gpu_roundtrip=sum(x["ms"] for x in segments if x["category"] == "observed_gpu_roundtrip"), no_block_host_elapsed=sum(x["ms"] for x in segments if x["category"] == "label_no_block")), causal_ambiguities=list(dict.fromkeys(ambiguities)), frame=present["value"], start_ns=start, end_ns=stop, elapsed_ms=(stop-start)/1e6, segments=segments, composition=[dict(tid=k[0], thread=thread_name(trace, k[0]), category=k[1], label=k[2], ms=v) for k, v in sorted(composition.items())], schedule_slack="unprovable: scheduler/resource and complete dependency constraints are not recorded", observed_offpath_headroom=headroom)


def analyze(trace, skip_frames=None, frames=None):
    if skip_frames is None:
        skip_frames = 0 if trace["windowed"] else 10
    if skip_frames < 0 or (frames is not None and frames <= 0):
        raise ValueError("skip_frames must be nonnegative and frames must be positive")
    presents = trace["presents"]
    first = max(1, skip_frames)
    selected = presents[first:first + frames if frames is not None else None]
    results = [walk_frame(trace, presents[first+i-1]["ns"], event) for i, event in enumerate(selected)]
    totals = collections.defaultdict(float)
    for frame in results:
        for item in frame["composition"]:
            totals[(item["tid"], item["category"], item["label"])] += item["ms"]
    count = len(results)
    mutex_totals = collections.defaultdict(float)
    for frame in results:
        for segment in frame["segments"]:
            if segment["category"] in {"mutex_held", "mutex_wait_uncovered"}:
                mutex_totals[(segment["category"], segment.get("holder_tid", 0), segment["label"])] += segment["ms"]
    mutex_summary = [dict(category=category, holder_tid=tid or None, holder_thread=thread_name(trace, tid) if tid else "UNKNOWN", holder_phase=phase, average_ms=elapsed/count) for (category, tid, phase), elapsed in sorted(mutex_totals.items())]
    pending_producers = collections.defaultdict(float)
    for frame in results:
        for segment in frame["segments"]:
            if segment["category"] == "label_wait_elapsed_before_record":
                pending_producers[(segment["tid"], segment.get("eventual_producer_tid"))] += segment["ms"]
    pending_summary = [dict(waiter_tid=waiter, waiter_thread=thread_name(trace, waiter), eventual_producer_tid=producer, eventual_producer_thread=thread_name(trace, producer) if producer else "UNKNOWN", average_ms=elapsed/count, relationship="exact eventual producer recorded after this path endpoint; not a traversed edge; elapsed remains attributed to waiter") for (waiter, producer), elapsed in sorted(pending_producers.items())]
    outcomes = collections.Counter(event["name"] for event in trace["events"].values() if event["kind"] == "wait_end" and event["name"].startswith("label."))
    split = {key: sum(frame["label_path_split_ms"][key] for frame in results) / count if count else None for key in ("waiter_elapsed_before_record", "producer_path_elapsed", "submit_delay", "observed_gpu_roundtrip", "no_block_host_elapsed")}
    return dict(pipe_predecessor_policy="For pipe.commit.begin, select the later of exact enqueue and lifecycle-correlated previous retirement on the same committer/pipeline (retirement wins ties). Verify exact enqueue peers, paired begin/retire lifecycles, consecutive retirement ordinals and sequence continuity; enqueue profiling counters may reset and are not identities; missing or ambiguous identity stays UNKNOWN. This is an observed predecessor attribution, not complete scheduler/resource dominance.", pipe_predecessor_counts=dict(collections.Counter(source["name"] for source in trace["pipe_predecessors"].values())), mutex_policy="Waiter-preserving attribution: charge recorded acquisition wait overlaps only to proven holder acquisition-to-release intervals and holder phases, then continue on the waiter before wait_begin. Uncovered portions remain UNKNOWN. This does not prove the waiter would have run absent contention; scheduler/runnable intervals are unavailable. Observed off-path headroom is based on walked event membership, not a complete scheduling analysis.", average_mutex_holder_composition=mutex_summary, proven_mutex_holds=sum(len(holds) for holds in trace["mutex_holds"].values()), average_pending_label_producers=pending_summary, label_wait_outcomes=dict(outcomes), average_label_path_split_ms=split, label_split_interpretation="Selected observed path only, not total waits or full critical path. Waiter elapsed before record is an in-flight wait crossed through another causal endpoint; its eventual producer is in the future and cannot be traversed. It can include host work and blocking. Producer path elapsed follows the exact selected producer and may contain nested waits; it excludes the separately listed waiter-before-record, submit, roundtrip and no-block categories and is not CPU execution. Other path intervals remain in the full composition. Submit delay is exact label record to CPU submit begin. Observed GPU roundtrip is CPU submit begin to successful CPU observation, INCLUDING CPU submit call, queueing, driver and host-observation delays; GPU execution duration is UNKNOWN. No-block elapsed is host checking, not blocking. Categories do not establish schedule slack.", capture_window=trace["window"], startup_history_omitted=trace["windowed"], cap_reached=trace["cap_reached"], cap_dropped=trace["cap_dropped"], thread_name_truncation=trace["metadata_name_loss"], schema=SCHEMA, assessment="observed_partial", completeness_proven=False, missing_constraints=["arbitrary guest/title synchronization and work dependencies", "GPU execution/completion causal timeline on the shared clock", "scheduler runnable/running intervals and CPU/GPU resource contention"], name_truncation=trace["name_loss"], unregistered_events=trace["unregistered_events"], coverage=trace["header"]["coverage"], warnings=trace["warnings"], truncated=trace["loss"] or bool(trace["gaps"]) or bool(trace["dropped"]), unknown_edges=len(trace["unknown"]), skipped_presents=first, selected_frames=count, frames=results, average_elapsed_ms=sum(x["elapsed_ms"] for x in results)/count if count else None, average_composition=[dict(tid=k[0], thread=thread_name(trace, k[0]), category=k[1], label=k[2], ms=v/count) for k,v in sorted(totals.items())], interpretation="Observed partial causal path only; complete application/GPU coverage is not established. Elapsed phase time is not CPU execution time. UNKNOWN and uninstrumented time are not CPU work. Observed headroom is not schedule slack.")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Walk recorded causal frame paths; never infer same-object wake edges.")
    parser.add_argument("trace", type=Path)
    parser.add_argument("--skip-frames", type=int, default=None, help="skip first N present markers (default 0 for window-only captures, otherwise 10); first marker is always a boundary")
    parser.add_argument("--frames", type=int)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        opener = gzip.open if args.trace.suffix.lower() == ".gz" else open
        with opener(args.trace, "rt", encoding="utf-8") as stream:
            report = analyze(read_trace(stream), args.skip_frames, args.frames)
    except (OSError, EOFError, ValueError, KeyError) as error:
        parser.exit(2, f"critical_path: {error}\n")
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"Selected {report['selected_frames']} frames; average elapsed {report['average_elapsed_ms']} ms")
        print("Assessment: observed partial causal path; completeness is not proven.")
        print("Coverage: " + ", ".join(report["coverage"]))
        print(f"Truncated: {report['truncated']}; unknown edges: {report['unknown_edges']}")
        for warning in report["warnings"]:
            print("WARNING: " + warning)
        for item in report["average_composition"]:
            print(f"{item['thread']}: {item['category']} {item['label']}: {item['ms']:.6f} ms/frame")
        for ambiguity in sorted({item for frame in report["frames"] for item in frame["causal_ambiguities"]}):
            print("CAUSAL UNCERTAINTY: " + ambiguity)
        print(report["mutex_policy"])
        print("Mutex holders: " + json.dumps(report["average_mutex_holder_composition"]))
        print("Pending label relationships: " + json.dumps(report["average_pending_label_producers"]))
        print(report["label_split_interpretation"])
        print("Label path split ms/frame: " + json.dumps(report["average_label_path_split_ms"]))
        print(report["interpretation"])
        print("Schedule slack: unprovable without scheduler/resource and complete dependency constraints.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
