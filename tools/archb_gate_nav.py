import argparse
from collections import deque
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

from archb_gate_config import GateError, atomic_bytes, sha256, write_json


PROMPT_BOX = (250, 205, 390, 250)
ACTIONS = {"intro_logo": ("cross", 1500), "story": ("cross", 1500),
           "bluepoint": ("cross", 500), "menu_newgame": ("cross", 500),
           "online_prompt_offline": ("cross", 500), "online_prompt_online": ("down", 250),
           "body_type": ("cross", 500)}
TARGETS = {"name_screen", "name_screen_class"}


def feature(image, box=None):
    gray = image.convert("L")
    if box:
        width, height = gray.size
        gray = gray.crop(tuple(round(value * (width / 640 if index % 2 == 0 else height / 360)) for index, value in enumerate(box)))
    values = np.asarray(gray.resize((160, 90) if box is None else (70, 22)), dtype=np.float32)
    values = values - values.mean()
    norm = np.linalg.norm(values)
    return values / norm if norm else values


def load_refs(directory):
    result = {}
    for path in sorted(Path(directory).glob("*.bmp")):
        with Image.open(path) as image:
            result[path.stem] = (feature(image), feature(image, PROMPT_BOX))
    required = set(ACTIONS) | TARGETS | {"class_list"}
    if not required <= result.keys():
        raise GateError(11, f"missing navigation references: {sorted(required - result.keys())}")
    return result


def classify(image, refs):
    if image is None:
        return "loading", 0.0
    luma = np.asarray(image.convert("L"), dtype=np.float32)
    if float(luma.mean()) < 2 or float(luma.std()) < 1:
        return "loading", 0.0
    full = feature(image)
    score, name = max((float((full * reference[0]).sum()), name) for name, reference in refs.items())
    if score < 0.80:
        return "unknown", score
    if name.startswith("online_prompt"):
        crop = feature(image, PROMPT_BOX)
        crop_score, name = max((float((crop * refs[name][1]).sum()), name)
                               for name in ("online_prompt_online", "online_prompt_offline"))
        score = min(score, crop_score)
    return (name if score >= 0.80 else "unknown"), score


class DriverCapture:
    def __init__(self, control, shots, timeout=5):
        self.control = Path(control)
        self.shots = Path(shots)
        self.timeout = timeout
        self.shots.mkdir(parents=True, exist_ok=True)

    def image(self):
        existing = set(self.shots.glob("*.png"))
        requested = time.time_ns()
        atomic_bytes(self.control, f"{requested // 1000} SCREENSHOT=1\n".encode("ascii"))
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            files = sorted(self.shots.glob("*.png"), key=lambda path: (path.stat().st_mtime_ns, path.name))
            for path in reversed(files):
                if path in existing or path.stat().st_mtime_ns <= requested:
                    continue
                try:
                    with Image.open(path) as source:
                        image = source.convert("RGB")
                except OSError:
                    continue
                for old in files[:-5]:
                    old.unlink(missing_ok=True)
                return image
            time.sleep(0.05)
        return None


class Capture:
    def __init__(self, pid):
        if os.name != "nt":
            raise GateError(11, "window capture requires Windows")
        self.pid = pid
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        self.callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        definitions = ((self.user, "EnumWindows", [self.callback, wintypes.LPARAM], wintypes.BOOL),
                       (self.user, "GetWindowThreadProcessId", [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD),
                       (self.user, "IsWindowVisible", [wintypes.HWND], wintypes.BOOL),
                       (self.user, "GetClientRect", [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL),
                       (self.user, "GetDC", [wintypes.HWND], wintypes.HDC),
                       (self.user, "ReleaseDC", [wintypes.HWND, wintypes.HDC], ctypes.c_int),
                       (self.user, "PrintWindow", [wintypes.HWND, wintypes.HDC, wintypes.UINT], wintypes.BOOL),
                       (self.gdi, "CreateCompatibleDC", [wintypes.HDC], wintypes.HDC),
                       (self.gdi, "CreateCompatibleBitmap", [wintypes.HDC, ctypes.c_int, ctypes.c_int], wintypes.HBITMAP),
                       (self.gdi, "SelectObject", [wintypes.HDC, wintypes.HANDLE], wintypes.HANDLE),
                       (self.gdi, "DeleteObject", [wintypes.HANDLE], wintypes.BOOL),
                       (self.gdi, "DeleteDC", [wintypes.HDC], wintypes.BOOL),
                       (self.gdi, "PatBlt", [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.DWORD], wintypes.BOOL),
                       (self.gdi, "BitBlt", [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                             wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD], wintypes.BOOL),
                       (self.gdi, "GetDIBits", [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                                              wintypes.LPVOID, wintypes.LPVOID, wintypes.UINT], ctypes.c_int))
        for library, name, arguments, result in definitions:
            function = getattr(library, name)
            function.argtypes = arguments
            function.restype = result
        self.user.SetProcessDPIAware()

    def window(self):
        matches = []
        def visit(hwnd, unused):
            owner = wintypes.DWORD()
            self.user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            rect = wintypes.RECT()
            if owner.value == self.pid and self.user.IsWindowVisible(hwnd) and self.user.GetClientRect(hwnd, ctypes.byref(rect)):
                width, height = rect.right, rect.bottom
                if width > 0 and height > 0:
                    matches.append((width * height, hwnd, width, height))
            return True
        self.user.EnumWindows(self.callback(visit), 0)
        if not matches:
            raise GateError(11, f"no visible game window for PID {self.pid}")
        return max(matches)[1:]

    def image(self):
        hwnd, width, height = self.window()
        source = self.user.GetDC(hwnd)
        memory = self.gdi.CreateCompatibleDC(source)
        bitmap = self.gdi.CreateCompatibleBitmap(source, width, height)
        if not source or not memory or not bitmap:
            if bitmap:
                self.gdi.DeleteObject(bitmap)
            if memory:
                self.gdi.DeleteDC(memory)
            if source:
                self.user.ReleaseDC(hwnd, source)
            raise GateError(11, "cannot create window capture bitmap")
        previous = self.gdi.SelectObject(memory, bitmap)
        try:
            self.gdi.PatBlt(memory, 0, 0, width, height, 0x00000042)
            self.user.PrintWindow(hwnd, memory, 3)
            def pixels():
                self.gdi.SelectObject(memory, previous)
                header = ctypes.create_string_buffer(40)
                import struct
                struct.pack_into("<IiiHHIIiiII", header, 0, 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
                data = ctypes.create_string_buffer(width * height * 4)
                copied = self.gdi.GetDIBits(memory, bitmap, 0, height, data, header, 0)
                self.gdi.SelectObject(memory, bitmap)
                if copied != height:
                    raise GateError(11, "GetDIBits failed")
                return Image.frombytes("RGB", (width, height), data.raw, "raw", "BGRX")
            image = pixels()
            if float(np.asarray(image.convert("L")).mean()) < 2:
                if not self.gdi.BitBlt(memory, 0, 0, width, height, source, 0, 0, 0x00CC0020):
                    raise GateError(11, "client BitBlt failed")
                image = pixels()
            return image
        finally:
            self.gdi.SelectObject(memory, previous)
            self.gdi.DeleteObject(bitmap)
            self.gdi.DeleteDC(memory)
            self.user.ReleaseDC(hwnd, source)


def press(pad, button, milliseconds, output):
    process = subprocess.run([sys.executable, str(Path(__file__).with_name("pad_input.py")), str(pad), button,
                              "--hold-ms", str(milliseconds)], capture_output=True, text=True)
    with (Path(output) / "input.log").open("a", encoding="utf-8") as log:
        log.write(f"{time.time():.3f} {button} {milliseconds}ms {process.stdout.strip()} {process.stderr.strip()}\n")
    if process.returncode:
        raise GateError(11, "pad_input.py failed")


def navigate(pid, refs_directory, pad, output, timeout=1500, capture="driver", control=None, shots=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    refs = load_refs(refs_directory)
    capture = Capture(pid) if capture == "window" else DriverCapture(control or output / "control.txt", shots or output / "shots")
    started = time.monotonic()
    deadline = started + timeout
    unknown_since = started
    streak_state, streak, last_capture = None, 0, -1.0
    action_state, action_deadline, circle_deadline = None, 0.0, None
    recent = deque(maxlen=5)
    scores_seen = {}
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if circle_deadline is not None and now >= circle_deadline:
                raise GateError(12, "Circle did not return class_list to name_screen within 10s")
            try:
                image = capture.image()
                if image is not None:
                    recent.append(image)
                state, score = classify(image, refs)
            except GateError:
                image, state, score = None, "unknown", 0.0
            now = time.monotonic()
            scores_seen[state] = max(scores_seen.get(state, -1), score)
            with (output / "nav.log").open("a", encoding="utf-8") as log:
                log.write(f"{now - started:.3f} {state} {score:.6f}\n")
            if state == "loading":
                streak_state, streak = None, 0
                unknown_since = now
            elif state == "unknown":
                streak_state, streak = None, 0
                if now - unknown_since >= 60:
                    raise GateError(11, "unknown screen for 60s; no input sent")
            else:
                unknown_since = now
                if state != streak_state:
                    streak_state, streak = state, 0
                if now - last_capture >= 1:
                    streak += 1
                    last_capture = now
                if streak >= 3:
                    if (circle_deadline is None and state in TARGETS) or (circle_deadline is not None and state == "name_screen"):
                        press(pad, "release", 1, output)
                        image.save(output / "target.png")
                        result = {"state": state, "score": score, "T_nav": time.monotonic() - started,
                                  "reference_scores": scores_seen, "reference_hashes": {path.name: sha256(path) for path in sorted(Path(refs_directory).glob("*.bmp"))}}
                        write_json(output / "navigation.json", result)
                        return result
                    if state == "class_list" and circle_deadline is None:
                        press(pad, "circle", 500, output)
                        circle_deadline = time.monotonic() + 10
                    elif circle_deadline is None and state in ACTIONS and (state != action_state or now >= action_deadline):
                        button, milliseconds = ACTIONS[state]
                        press(pad, button, milliseconds, output)
                        action_state, action_deadline = state, time.monotonic() + 20
                        streak = 0
            time.sleep(1)
        raise GateError(13, "global navigation timeout (1500s)")
    except GateError:
        for index, image in enumerate(recent):
            image.save(output / f"unknown-{index}.png")
        raise


def contact_sheet(directory):
    directory = Path(directory)
    files = sorted(list(directory.rglob("start.png")) + list(directory.rglob("end.png"))
                   + list(directory.rglob("last-control.png")) + list(directory.rglob("last-candidate.png")))
    if not files:
        return {"screenshots": 0}
    sheet = Image.new("RGB", (640, ((len(files) + 1) // 2) * 205), "#202020")
    draw = ImageDraw.Draw(sheet)
    for index, path in enumerate(files):
        x, y = (index % 2) * 320, (index // 2) * 205
        with Image.open(path) as image:
            sheet.paste(image.resize((320, 180)), (x, y))
        draw.text((x + 4, y + 183), str(path.relative_to(directory)), fill="white")
    sheet.save(directory / "contact.png")
    return {"screenshots": len(files)}


def presented_events(path):
    events = []
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return events
    for line in text.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict) and event.get("ev") == "present" and type(event.get("t_ns")) is int and type(event.get("presented")) is int:
            events.append((event["t_ns"], event["presented"]))
    return events


def window_rates(events, seconds=10):
    if len(events) < 2:
        return []
    width = seconds * 1_000_000_000
    rates = []
    end = events[-1][0]
    while len(rates) < 3:
        start = end - width
        inside = [event for event in events if start <= event[0] <= end]
        if len(inside) < 2 or events[0][0] > start:
            break
        rates.append((inside[-1][1] - inside[0][1]) * 1e9 / (inside[-1][0] - inside[0][0]))
        end = start
    return list(reversed(rates))


def steady(rates, scene_max_fps):
    if len(rates) < 3 or any(rate < 0.5 or rate >= scene_max_fps for rate in rates):
        return False
    return all(abs(after - before) <= 0.2 * max(before, after) for before, after in zip(rates, rates[1:]))


def navigate_continue(pid, pad, output, timeout=1500, capture="driver", control=None, shots=None, scene_max_fps=10.0):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    telemetry = output / "videoout.jsonl"
    started = time.monotonic()
    deadline = started + timeout
    first_frame = None
    opening = []
    presses = 0
    last_press = None
    with (output / "nav.log").open("a", encoding="utf-8") as log:
        while time.monotonic() < deadline:
            now = time.monotonic()
            events = presented_events(telemetry)
            if first_frame is None and events:
                first_frame = now
                opening = [first_frame + 20, first_frame + 32]
            rates = window_rates(events)
            log.write(f"{now - started:.3f} continue presented={events[-1][1] if events else 0} rates={[round(rate, 2) for rate in rates]}\n")
            log.flush()
            if opening and now >= opening[0]:
                opening.pop(0)
                press(pad, "cross", 1500, output)
                presses, last_press = presses + 1, now
            elif not opening and first_frame is not None:
                if steady(rates, scene_max_fps):
                    press(pad, "release", 1, output)
                    image = (Capture(pid) if capture == "window" else DriverCapture(control or output / "control.txt", shots or output / "shots")).image()
                    if image is not None:
                        image.save(output / "target.png")
                    result = {"state": "continue_scene", "rates": rates, "presses": presses, "T_nav": time.monotonic() - started}
                    write_json(output / "navigation.json", result)
                    return result
                if rates and rates[-1] >= scene_max_fps and presses < 8 and now - last_press > 60:
                    press(pad, "cross", 1500, output)
                    presses, last_press = presses + 1, now
            time.sleep(2)
    raise GateError(13, f"global navigation timeout ({timeout}s)")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    nav = sub.add_parser("navigate")
    nav.add_argument("--pid", type=int, required=True)
    nav.add_argument("--refs", required=True)
    nav.add_argument("--pad", required=True)
    nav.add_argument("--output", required=True)
    nav.add_argument("--route", choices=("continue", "creator"), default="continue")
    nav.add_argument("--scene-max-fps", type=float, default=10.0)
    screenshot = sub.add_parser("screenshot")
    screenshot.add_argument("--pid", type=int, required=True)
    screenshot.add_argument("--output", required=True)
    for command in (nav, screenshot):
        command.add_argument("--capture", choices=("driver", "window"), default="driver")
        command.add_argument("--control")
        command.add_argument("--shots")
    contact = sub.add_parser("contact")
    contact.add_argument("directory")
    args = parser.parse_args()
    try:
        if args.command == "navigate" and args.route == "continue":
            result = navigate_continue(args.pid, args.pad, args.output, capture=args.capture, control=args.control, shots=args.shots,
                                       scene_max_fps=args.scene_max_fps)
        elif args.command == "navigate":
            result = navigate(args.pid, args.refs, args.pad, args.output, capture=args.capture, control=args.control, shots=args.shots)
        elif args.command == "screenshot":
            directory = Path(args.output).parent
            capture = Capture(args.pid) if args.capture == "window" else DriverCapture(args.control or directory / "control.txt", args.shots or directory / "shots")
            image = capture.image()
            if image is None:
                raise GateError(11, "no driver screenshot within 5s")
            image.save(args.output)
            result = {"screenshot": args.output}
        else:
            result = contact_sheet(args.directory)
        print(json.dumps(result))
        return 0
    except (GateError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return error.code if isinstance(error, GateError) else 11


if __name__ == "__main__":
    sys.exit(main())
