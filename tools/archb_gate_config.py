import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path


class GateError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    atomic_bytes(path, (json.dumps(value, indent=2, allow_nan=False) + "\n").encode("utf-8"))


def atomic_bytes(path, data):
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        for attempt in range(60):
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                if attempt == 59:
                    raise
                time.sleep(0.5)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def measured_environment(env):
    for key in env:
        upper = key.upper()
        if upper.startswith("APS5_ARCHB_"):
            continue
        if (upper.startswith(("APS5_PROFILE_", "APS5_TRACE_", "VVL_"))
                or any(word in upper for word in ("CAPTURE", "DUMP", "VALIDATION"))
                or upper in ("VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES", "VK_LAYER_SETTINGS_PATH")):
            raise GateError(24, f"forbidden measured environment: {key}")


def child_environment(inherited, manifest, leg, kind="measured"):
    configured = manifest["env"]
    measured_environment(configured)
    result = {key: value for key, value in inherited.items()
              if not key.upper().startswith(("APS5_", "ANYPS5_", "VK_", "VVL_"))}
    result.update(configured)
    result.update(APS5_PAD_INPUT_FILE=str(Path(leg) / "pad.txt"),
                  APS5_VIDEOOUT_TELEMETRY_DIR=str(leg),
                  APS5_ARCHB_CONTROL_FILE=str(Path(leg) / "control.txt"),
                  ANYPS5_SCREENSHOTS=str(Path(leg) / "shots"),
                  ANYPS5_SHADER_CACHE_DIR=manifest["shaderCache"], ANYPS5_OVERLAY="1")
    if kind == "diagnostic":
        result.update(APS5_PROFILE_DRAW="1", APS5_PROFILE_DRAW_SHADERS="1", APS5_PROFILE_SHADERS_REPORT="1",
                      APS5_TRACE_SUMMARY_EVERY="10", APS5_TRACE_DIR=str(leg))
    elif kind == "validation":
        result.update(VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                      VK_LAYER_ENABLES="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
    elif kind != "measured":
        raise GateError(24, f"unknown leg kind: {kind}")
    return result


def validate_manifest(value):
    required = {"name", "build", "libs", "env", "shaderCache", "requiredCounters", "expectSwitches"}
    if not isinstance(value, dict) or not required <= set(value) or set(value) - required - {"allocatorLedgerBytes"}:
        raise GateError(23, f"manifest needs {sorted(required)} and optional allocatorLedgerBytes")
    if "allocatorLedgerBytes" in value and (type(value["allocatorLedgerBytes"]) is not int or value["allocatorLedgerBytes"] < 0):
        raise GateError(26, "allocatorLedgerBytes must be a nonnegative integer")
    if not isinstance(value["name"], str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value["name"]):
        raise GateError(23, "unsafe configuration name")
    for key in ("build", "shaderCache"):
        if not isinstance(value[key], str) or not Path(value[key]).is_absolute():
            raise GateError(23, f"{key} must be absolute")
    if not isinstance(value["env"], dict) or any(not isinstance(key, str) or not isinstance(item, str)
                                               or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key)
                                               or "\0" in item for key, item in value["env"].items()):
        raise GateError(24, "env must map variable names to strings")
    measured_environment(value["env"])
    if value["env"].get("APS5_ARCHB_GATE_RUN") != "1":
        raise GateError(24, "a gate requires APS5_ARCHB_GATE_RUN=1")
    if not isinstance(value["libs"], dict) or not {"libSceAgcDriver.prx", "libSceVideoOut.prx"} <= value["libs"].keys():
        raise GateError(23, "both driver and VideoOut hashes are required")
    for name, digest in value["libs"].items():
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_]+\.prx", name) or not isinstance(digest, str) or not re.fullmatch(r"[a-fA-F0-9]{64}", digest):
            raise GateError(23, "invalid library name or SHA-256")
    if not isinstance(value["requiredCounters"], list) or any(not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_]\w*", name) for name in value["requiredCounters"]):
        raise GateError(22, "invalid required counter names")
    if not isinstance(value["expectSwitches"], dict) or any(not isinstance(key, str) or not re.fullmatch(r"[A-Z][A-Z0-9_]*", key)
            or not isinstance(item, str) or not re.fullmatch(r"[01]" if key != "GATE_DELAY_US" else r"[0-9]+", item)
            for key, item in value["expectSwitches"].items()):
        raise GateError(21, "invalid expectSwitches")
    return value


def switch_metadata(text):
    lines = re.findall(r"^\[archb\] switches(.*)$", text, re.M)
    if len(lines) != 1:
        raise GateError(21, f"expected one switches line, got {len(lines)}")
    result = {}
    for token in lines[0].split():
        match = re.fullmatch(r"([A-Z][A-Z0-9_]*)=([0-9]+)\((default|env)\)(\(rt\))?", token)
        if match is None or match[1] in result or (match[1] != "GATE_DELAY_US" and match[2] not in ("0", "1")):
            raise GateError(21, f"invalid switches token: {token}")
        result[match[1]] = {"value": match[2], "runtime": bool(match[4])}
    return result


def switch_assertion(text, expected):
    if not expected and not re.search(r"^\[archb\] switches", text, re.M):
        return {}
    actual = {name: entry["value"] for name, entry in switch_metadata(text).items()}
    if any(actual.get(key) != value for key, value in expected.items()):
        raise GateError(21, f"switch mismatch: expected {expected}, observed {actual}")
    return actual


def alternating_plan(control, candidate, control_text, candidate_text, bootstrap=False):
    left, right = switch_metadata(control_text), switch_metadata(candidate_text)
    if (os.path.normcase(str(Path(control["build"]).resolve())) != os.path.normcase(str(Path(candidate["build"]).resolve()))
            or control["libs"] != candidate["libs"] or left.keys() != right.keys()
            or any(left[name]["runtime"] != right[name]["runtime"] for name in left)):
        return {"eligible": False, "reason": "different build, libraries or switch table"}
    for key in control["env"].keys() | candidate["env"].keys():
        if control["env"].get(key) == candidate["env"].get(key):
            continue
        name = key.removeprefix("APS5_ARCHB_")
        if not key.startswith("APS5_ARCHB_") or not left.get(name, {}).get("runtime"):
            return {"eligible": False, "reason": f"startup environment differs: {key}"}
    if any(left[name]["value"] != right[name]["value"] and not left[name]["runtime"] for name in left):
        return {"eligible": False, "reason": "startup switch differs"}
    result = {"eligible": True, "metadata": right,
              "control": {name: entry["value"] for name, entry in left.items() if entry["runtime"]},
              "candidate": {name: entry["value"] for name, entry in right.items() if entry["runtime"]}}
    if bootstrap:
        if not right.get("GATE_NOOP", {}).get("runtime"):
            raise GateError(21, "alternating Bootstrap requires runtime GATE_NOOP")
        result["control"]["GATE_NOOP"] = "0"
        result["candidate"]["GATE_NOOP"] = "1"
    if not result["control"]:
        return {"eligible": False, "reason": "no runtime switches"}
    return result


def counter_assertion(text, required):
    counters = {}
    for line in text.splitlines():
        if "[archb]" not in line:
            continue
        for name, value in re.findall(r"\b([A-Za-z_]\w*)\s*[=:]\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))", line):
            counters[name] = max(counters.get(name, 0), float(value))
    missing = [name for name in required if counters.get(name, 0) <= 0]
    if missing:
        raise GateError(22, "required counters missing or zero: " + ", ".join(missing))
    return counters


def module_assertion(modules, libs, runtime):
    result = {}
    for name, expected in libs.items():
        matches = [module for module in modules if Path(module["path"]).name.lower() == name.lower()]
        target = Path(runtime) / "libs" / name
        if len(matches) != 1 or os.path.normcase(str(Path(matches[0]["path"]).resolve())) != os.path.normcase(str(target.resolve())):
            raise GateError(23, f"loaded module path mismatch for {name}: {matches}")
        actual = sha256(matches[0]["path"])
        if actual.lower() != expected.lower() or matches[0].get("sha256", actual).lower() != expected.lower():
            raise GateError(23, f"loaded module hash mismatch for {name}")
        result[name] = {"path": str(target), "sha256": actual}
    return result


def validation_ids(text):
    return sorted(set(re.findall(r"\b(?:VUID|SYNC)-[A-Za-z0-9_.-]+", text)))


def validation_cache_key(manifest):
    control = manifest["configurations"]["control"]
    identity = {"build": os.path.normcase(str(Path(control["build"]).resolve())), "libs": control["libs"], "env": control["env"],
                **{key: manifest[key] for key in ("gpu", "display", "windows")}}
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode("utf-8")).hexdigest()


def revision(build):
    def git(*args):
        process = subprocess.run(["git", "-C", str(build), *args], capture_output=True, text=True, check=True)
        return process.stdout.strip()
    return {"revision": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain"))}


def prepare(control, candidates, mode):
    control = validate_manifest(read_json(control))
    candidates = [validate_manifest(read_json(path)) for path in candidates]
    if not candidates:
        raise GateError(23, "no candidates")
    names = [control["name"]] + [candidate["name"] for candidate in candidates]
    caches = [os.path.normcase(str(Path(item["shaderCache"]).resolve())) for item in [control] + candidates]
    if len(set(names)) != len(names) or len(set(caches)) != len(caches):
        raise GateError(23, "each configuration needs a distinct name and shaderCache")
    if any(Path(left).is_relative_to(Path(right)) for i, left in enumerate(caches) for j, right in enumerate(caches) if i != j):
        raise GateError(23, "shader caches must not nest")
    if mode == "Bootstrap":
        if len(candidates) != 1 or any(control[key] != candidates[0][key] for key in control if key not in ("name", "shaderCache")):
            raise GateError(23, "Bootstrap is A/A: configurations may differ only in name and shaderCache")
    if any(set(item["libs"]) != set(control["libs"]) for item in candidates):
        raise GateError(23, "control and candidates must list the same deployed libraries")
    for item in [control] + candidates:
        item["revision"] = revision(item["build"])
        item["sources"] = {}
        for name, digest in item["libs"].items():
            patched = Path(item["build"]) / "core/libs/libs" / name
            unpatched = patched.parent / "unpatched" / name
            if sha256(patched).lower() != digest.lower():
                raise GateError(23, f"build hash mismatch: {patched}")
            item["sources"][name] = {"patched": str(patched), "unpatched": str(unpatched),
                                      "unpatched_sha256": sha256(unpatched)}
    video_hashes = {item["libs"]["libSceVideoOut.prx"].lower() for item in candidates}
    if len(video_hashes) != 1:
        raise GateError(23, "matrix candidates must share the full-rate VideoOut build")
    control["originalVideoOut"] = {"sha256": control["libs"]["libSceVideoOut.prx"], "source": control["sources"]["libSceVideoOut.prx"]}
    control["libs"]["libSceVideoOut.prx"] = candidates[0]["libs"]["libSceVideoOut.prx"]
    control["sources"]["libSceVideoOut.prx"] = candidates[0]["sources"]["libSceVideoOut.prx"]
    return {"control": control, "candidates": candidates, "mode": mode}


def restore_targets(runtime, libraries):
    runtime = Path(runtime).absolute()
    names = sorted(set(libraries))
    if any(not re.fullmatch(r"[A-Za-z0-9_]+\.prx", name) for name in names):
        raise GateError(31, "unsafe restore library name")
    return [runtime / "libs" / name for name in names] + [runtime / "libs/unpatched" / name for name in names] + [runtime / "app0/PackageCmdLineArgs.txt"]


def snapshot(runtime, libraries, manifest):
    entries = []
    for path in restore_targets(runtime, libraries):
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise GateError(31, f"restore target must be a regular file: {path}")
        data = path.read_bytes() if path.exists() else None
        entries.append({"path": str(path), "exists": data is not None, "sha256": hashlib.sha256(data).hexdigest() if data is not None else None,
                        "bytes": base64.b64encode(data).decode("ascii") if data is not None else None})
    plan = {"status": "pending", "runtime": str(Path(runtime).absolute()), "libraries": sorted(set(libraries)), "entries": entries}
    write_json(manifest, plan)
    return plan


def restore(manifest):
    plan = read_json(manifest)
    if plan["status"] == "restored":
        return plan
    expected = restore_targets(plan["runtime"], plan["libraries"])
    if [str(path) for path in expected] != [entry["path"] for entry in plan["entries"]]:
        raise GateError(31, "restore target list was changed")
    for entry in plan["entries"]:
        path = Path(entry["path"])
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise GateError(31, f"unsafe restore target: {path}")
        if entry["exists"]:
            data = base64.b64decode(entry["bytes"], validate=True)
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise GateError(31, f"corrupt restore bytes: {path}")
            atomic_bytes(path, data)
        else:
            path.unlink(missing_ok=True)
    for entry in plan["entries"]:
        path = Path(entry["path"])
        if path.exists() != entry["exists"] or (entry["exists"] and sha256(path) != entry["sha256"]):
            raise GateError(31, f"restore verification failed: {path}")
    plan["status"] = "restored"
    plan["verified"] = [{key: entry[key] for key in ("path", "exists", "sha256")} for entry in plan["entries"]]
    write_json(manifest, plan)
    return plan


def repair_pending(runtime):
    pointer = Path(runtime) / "archb-gate-pending.json"
    if not pointer.exists():
        return {"status": "clean"}
    pending = read_json(pointer)
    plan = read_json(pending["manifest"])
    if Path(plan["runtime"]).resolve() != Path(runtime).resolve():
        raise GateError(31, "pending manifest is for another runtime")
    result = restore(pending["manifest"])
    pointer.unlink()
    return result


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("--control", required=True)
    prep.add_argument("--candidate", action="append", required=True)
    prep.add_argument("--mode", required=True)
    prep.add_argument("--output", required=True)
    env = sub.add_parser("environment")
    env.add_argument("manifest")
    env.add_argument("leg")
    env.add_argument("--kind", default="measured")
    snap = sub.add_parser("snapshot")
    snap.add_argument("runtime")
    snap.add_argument("manifest")
    snap.add_argument("libraries", nargs="+")
    recover = sub.add_parser("restore")
    recover.add_argument("manifest")
    repair = sub.add_parser("repair")
    repair.add_argument("runtime")
    verify = sub.add_parser("verify")
    verify.add_argument("manifest")
    verify.add_argument("directory")
    verify.add_argument("runtime")
    verify.add_argument("--diagnostic", action="store_true")
    alternate = sub.add_parser("alternating-plan")
    alternate.add_argument("control")
    alternate.add_argument("candidate")
    alternate.add_argument("control_log")
    alternate.add_argument("candidate_log")
    alternate.add_argument("--bootstrap", action="store_true")
    cache = sub.add_parser("validation-key")
    cache.add_argument("manifest")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            result = prepare(args.control, args.candidate, args.mode)
            write_json(args.output, result)
        elif args.command == "environment":
            result = child_environment(os.environ, read_json(args.manifest), args.leg, args.kind)
        elif args.command == "snapshot":
            result = snapshot(args.runtime, args.libraries, args.manifest)
            write_json(Path(args.runtime) / "archb-gate-pending.json", {"manifest": str(Path(args.manifest).absolute())})
        elif args.command == "restore":
            result = restore(args.manifest)
        elif args.command == "repair":
            result = repair_pending(args.runtime)
        elif args.command == "alternating-plan":
            result = alternating_plan(read_json(args.control), read_json(args.candidate),
                                      Path(args.control_log).read_text(encoding="utf-8", errors="replace"),
                                      Path(args.candidate_log).read_text(encoding="utf-8", errors="replace"), args.bootstrap)
        elif args.command == "validation-key":
            result = {"key": validation_cache_key(read_json(args.manifest))}
        else:
            manifest = read_json(args.manifest)
            directory = Path(args.directory)
            text = (directory / "stderr.log").read_text(encoding="utf-8", errors="replace")
            result = {"switches": switch_assertion(text, manifest["expectSwitches"]),
                      "modules": module_assertion(read_json(directory / "modules.json"), manifest["libs"], args.runtime)}
            if args.diagnostic:
                result["counters"] = counter_assertion(text, manifest["requiredCounters"])
                if "allocatorLedgerBytes" in manifest:
                    if "allocator_high_water_bytes" not in result["counters"]:
                        raise GateError(22, "driver allocator high-water is missing")
                    if result["counters"]["allocator_high_water_bytes"] > manifest["allocatorLedgerBytes"]:
                        raise GateError(26, "driver allocator high-water exceeds its ledger")
        if args.command in ("snapshot", "restore", "repair") and "entries" in result:
            result = {"status": result["status"], "runtime": result["runtime"],
                      "verified": [{key: entry[key] for key in ("path", "exists", "sha256")} for entry in result["entries"]]}
        print(json.dumps(result))
        return 0
    except (GateError, OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return error.code if isinstance(error, GateError) else 31 if args.command in ("restore", "repair", "snapshot") else 23


if __name__ == "__main__":
    sys.exit(main())
