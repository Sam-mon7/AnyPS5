import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tools"))

from archb_gate_config import GateError, alternating_plan, child_environment, counter_assertion, measured_environment, repair_pending, restore, sha256, snapshot, switch_assertion, validation_cache_key, write_json


def fails(call, code):
    try:
        call()
    except GateError as error:
        assert error.code == code, (error.code, code)
    else:
        raise AssertionError("expected gate failure")


def strips_inherited_aps5(directory):
    inherited = {"PATH": "system", "APS5_PROFILE_DRAW": "1", "aps5_OTHER": "1", "ANYPS5_OTHER": "1", "VK_OTHER": "1", "VVL_OTHER": "1"}
    original = inherited.copy()
    manifest = {"env": {"APS5_ARCHB_GATE_RUN": "1"}, "shaderCache": str(Path(directory) / "cache")}
    result = child_environment(inherited, manifest, directory)
    assert inherited == original
    assert result["PATH"] == "system" and result["APS5_ARCHB_GATE_RUN"] == "1"
    assert not any(key in result for key in original if key != "PATH")
    assert result["APS5_PAD_INPUT_FILE"] == str(Path(directory) / "pad.txt")
    assert result["ANYPS5_SHADER_CACHE_DIR"] == manifest["shaderCache"] and result["ANYPS5_OVERLAY"] == "1"
    assert child_environment({}, manifest, directory, "diagnostic")["APS5_PROFILE_DRAW_SHADERS"] == "1"
    assert child_environment({}, manifest, directory, "validation")["VK_INSTANCE_LAYERS"] == "VK_LAYER_KHRONOS_validation"
    for kind in ("measured", "diagnostic", "validation"):
        environment = child_environment(inherited, manifest, directory, kind)
        assert environment["APS5_ARCHB_CONTROL_FILE"] == str(Path(directory) / "control.txt")
        assert environment["ANYPS5_SCREENSHOTS"] == str(Path(directory) / "shots")
        assert not any("DUMP" in key for key in environment)


def rejects_profile_in_measured_leg(directory):
    for key in ("APS5_PROFILE_DRAW", "APS5_TRACE_SUMMARY_EVERY", "APS5_CAPTURE_FRAME", "APS5_DUMP_FRAMES", "VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES", "VVL_CONFIG"):
        fails(lambda: measured_environment({key: "0"}), 24)
    measured_environment({"APS5_ARCHB_UB_NO_LOCK_INVALIDATION": "1", "APS5_ARCHB_RECORDER": "1"})


def switch_line_mismatch_fails(directory):
    text = "[archb] switches GATE_RUN=1(env) DEPTH_PROOF=0(default)\n"
    assert switch_assertion(text, {"GATE_RUN": "1"}) == {"GATE_RUN": "1", "DEPTH_PROOF": "0"}
    fails(lambda: switch_assertion(text, {"GATE_RUN": "0"}), 21)
    fails(lambda: switch_assertion(text, {"MISSING": "1"}), 21)
    fails(lambda: switch_assertion(text + text, {"GATE_RUN": "1"}), 21)
    fails(lambda: switch_assertion("", {"GATE_RUN": "1"}), 21)


def required_counter_zero_fails(directory):
    text = "[archb] depth_overlap_clean_accepted=0 other=3\n"
    fails(lambda: counter_assertion(text, ["depth_overlap_clean_accepted"]), 22)
    fails(lambda: counter_assertion(text, ["does_not_exist"]), 22)
    assert counter_assertion(text + "[archb] depth_overlap_clean_accepted=7\n", ["depth_overlap_clean_accepted"])["depth_overlap_clean_accepted"] == 7


def alternating_eligibility(directory):
    control = {"build": directory, "libs": {"driver": "abc"}, "env": {"APS5_ARCHB_GATE_RUN": "1"}}
    candidate = {**control, "env": {**control["env"], "APS5_ARCHB_GATE_NOOP": "1", "APS5_ARCHB_GATE_DELAY_US": "7000"}}
    off = "[archb] switches GATE_RUN=1(env) GATE_NOOP=0(default)(rt) GATE_DELAY_US=0(default)(rt)\n"
    on = "[archb] switches GATE_RUN=1(env) GATE_NOOP=1(env)(rt) GATE_DELAY_US=7000(env)(rt)\n"
    result = alternating_plan(control, candidate, off, on)
    assert result["eligible"] and result["candidate"] == {"GATE_NOOP": "1", "GATE_DELAY_US": "7000"}
    assert switch_assertion(on, {"GATE_NOOP": "1", "GATE_DELAY_US": "7000"})["GATE_RUN"] == "1"
    assert not alternating_plan(control, {**candidate, "build": str(Path(directory) / "other")}, off, on)["eligible"]
    assert not alternating_plan(control, {**candidate, "libs": {"driver": "def"}}, off, on)["eligible"]
    assert not alternating_plan(control, candidate, off.replace("(rt)", ""), on.replace("(rt)", ""))["eligible"]
    assert not alternating_plan(control, {**candidate, "env": {**candidate["env"], "ANYPS5_GPU": "other"}}, off, on)["eligible"]
    assert not alternating_plan(control, {**candidate, "env": {**candidate["env"], "APS5_ARCHB_CONTROL_FILE": "other"}}, off, on)["eligible"]
    calibration = alternating_plan(control, control, off, off, True)
    assert calibration["eligible"] and calibration["control"]["GATE_NOOP"] == "0" and calibration["candidate"]["GATE_NOOP"] == "1"
    manifest = {"configurations": {"control": control}, "gpu": [{"name": "gpu", "driver": "1"}], "display": {"width": 640}, "windows": "test"}
    key = validation_cache_key(manifest)
    assert key == validation_cache_key(dict(reversed(list(manifest.items()))))
    assert key != validation_cache_key({**manifest, "gpu": [{"name": "gpu", "driver": "2"}]})


def fake_runtime(directory):
    runtime = Path(directory) / "runtime"
    for relative in ("libs", "libs/unpatched", "app0"):
        (runtime / relative).mkdir(parents=True, exist_ok=True)
    originals = {"libs/libSceAgcDriver.prx": b"patched\x00\xff", "libs/unpatched/libSceAgcDriver.prx": b"unpatched\xff\x00",
                 "app0/PackageCmdLineArgs.txt": b"args\r\n\x00"}
    for relative, data in originals.items():
        (runtime / relative).write_bytes(data)
    return runtime, originals


def restore_plan_round_trip(directory):
    runtime, originals = fake_runtime(directory)
    manifest = Path(directory) / "restore.json"
    snapshot(runtime, ["libSceAgcDriver.prx", "libSceVideoOut.prx"], manifest)
    for relative in originals:
        (runtime / relative).write_bytes(b"changed")
    (runtime / "libs/libSceVideoOut.prx").write_bytes(b"new")
    result = restore(manifest)
    assert result["status"] == "restored"
    for relative, data in originals.items():
        assert (runtime / relative).read_bytes() == data
    assert not (runtime / "libs/libSceVideoOut.prx").exists()
    assert not (runtime / "libs/unpatched/libSceVideoOut.prx").exists()
    assert restore(manifest) == result


def pending_manifest_repaired(directory):
    runtime, originals = fake_runtime(directory)
    manifest = Path(directory) / "restore.json"
    snapshot(runtime, ["libSceAgcDriver.prx"], manifest)
    write_json(runtime / "archb-gate-pending.json", {"manifest": str(manifest)})
    for relative in originals:
        (runtime / relative).unlink()
    result = repair_pending(runtime)
    assert result["status"] == "restored"
    assert not (runtime / "archb-gate-pending.json").exists()
    for relative, data in originals.items():
        assert (runtime / relative).read_bytes() == data
    assert repair_pending(runtime)["status"] == "clean"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="archb-config-") as directory:
        globals()[sys.argv[1]](directory)
    print(sys.argv[1] + " passed")
