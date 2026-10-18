import argparse
import base64
import bisect
from dataclasses import dataclass
from datetime import datetime
import html
import json
import math
from pathlib import Path
import re
import struct
import sys
import zlib

try:
    import numpy as np
except ImportError:
    np = None


@dataclass
class Image:
    width: int
    height: int
    rgb: bytes


@dataclass
class Frame:
    path: Path
    number: int
    timestamp: float
    metadata: dict


def extent(width, height):
    if not 0 < width <= 16384 or not 0 < height <= 16384:
        raise ValueError(f"invalid image dimensions: {width}x{height}")


def packed_image(data, width, height, pixel_format, stride=None, top_down=True):
    extent(width, height)
    if not isinstance(pixel_format, str):
        raise ValueError("pixel_format must be a string")
    channels = pixel_format.upper().removesuffix("8")
    if channels not in ("RGB", "BGR", "RGBA", "BGRA"):
        raise ValueError(f"unsupported pixel format: {pixel_format}")
    size = len(channels)
    stride = width * size if stride is None else stride
    if stride < width * size or len(data) != stride * height:
        raise ValueError("pixel buffer size/stride does not match dimensions")
    result = bytearray(width * height * 3)
    for y in range(height):
        source_y = y if top_down else height - y - 1
        row = data[source_y * stride:source_y * stride + width * size]
        for channel, name in enumerate("RGB"):
            result[y * width * 3 + channel:(y + 1) * width * 3:3] = row[channels.index(name)::size]
    return Image(width, height, bytes(result))


def read_bmp(data):
    if len(data) < 54:
        raise ValueError("truncated BMP header")
    offset, header_size = struct.unpack_from("<II", data, 10)
    width, height, planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    if header_size < 40 or offset < 14 + header_size or planes != 1 or bits not in (24, 32) or compression != 0:
        raise ValueError("BMP must be uncompressed 24-bit BGR or 32-bit BGRA")
    stride = ((width * bits + 31) // 32) * 4
    return packed_image(data[offset:], width, abs(height), "BGR" if bits == 24 else "BGRA", stride, height < 0)


def read_ppm(data):
    tokens = re.finditer(rb"#[^\r\n]*|[^\s#]+", data)
    header = []
    for token in tokens:
        if not token[0].startswith(b"#"):
            header.append(token[0])
            if len(header) == 4:
                end = token.end()
                break
    if len(header) != 4 or header[0] not in (b"P3", b"P6"):
        raise ValueError("invalid PPM header")
    width, height, maximum = map(int, header[1:])
    extent(width, height)
    if not 0 < maximum <= 255:
        raise ValueError("PPM requires an 8-bit maximum (1..255)")
    if header[0] == b"P3":
        samples = [int(token[0]) for token in tokens if not token[0].startswith(b"#")]
        if any(value < 0 or value > maximum for value in samples):
            raise ValueError("PPM sample outside declared range")
        pixels = bytes(samples)
    else:
        if data[end:end + 1] not in b" \t\r\n\v\f" or end == len(data):
            raise ValueError("missing PPM raster separator")
        end += 2 if data[end:end + 2] == b"\r\n" else 1
        pixels = data[end:]
        if any(value > maximum for value in pixels):
            raise ValueError("PPM sample outside declared range")
    if maximum != 255:
        pixels = bytes(round(value * 255 / maximum) for value in pixels)
    return packed_image(pixels, width, height, "RGB")


def paeth(a, b, c):
    p = a + b - c
    distances = (abs(p - a), abs(p - b), abs(p - c))
    return (a, b, c)[distances.index(min(distances))]


def read_png(data):
    position, compressed, palette, header = 8, bytearray(), None, None
    ended = False
    while position + 12 <= len(data):
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        body = data[position + 8:position + 8 + length]
        end = position + 8 + length
        if end + 4 > len(data) or zlib.crc32(kind + body) != struct.unpack_from(">I", data, end)[0]:
            raise ValueError("truncated PNG chunk or CRC mismatch")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IEND":
            ended = True
            break
        position = end + 4
    if not header or not ended:
        raise ValueError("missing PNG header/end")
    width, height, depth, color, compression, filtering, interlace = header
    extent(width, height)
    if depth != 8 or color not in (0, 2, 3, 4, 6) or compression or filtering or interlace:
        raise ValueError("PNG requires non-interlaced 8-bit samples")
    size = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    stride = width * size
    raw = zlib.decompress(compressed)
    if len(raw) != (stride + 1) * height:
        raise ValueError("PNG raster size does not match dimensions")
    pixels, previous = bytearray(), bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        method, row = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        if method > 4:
            raise ValueError("invalid PNG row filter")
        for x in range(stride):
            a, b = row[x - size] if x >= size else 0, previous[x]
            c = previous[x - size] if x >= size else 0
            predictor = (0, a, b, (a + b) // 2, paeth(a, b, c))[method]
            row[x] = (row[x] + predictor) & 255
        pixels.extend(row)
        previous = row
    if color == 3:
        if not palette or len(palette) % 3 or any(index * 3 + 3 > len(palette) for index in pixels):
            raise ValueError("invalid PNG palette/index")
        return Image(width, height, b"".join(palette[index * 3:index * 3 + 3] for index in pixels))
    if color in (0, 4):
        return Image(width, height, b"".join(bytes([value]) * 3 for value in pixels[::size]))
    return packed_image(pixels, width, height, "RGB" if color == 2 else "RGBA")


def read_image(frame):
    data = frame.path.read_bytes()
    if frame.path.suffix.lower() in (".raw", ".rgb", ".rgba", ".bgra"):
        metadata = frame.metadata
        if not all(key in metadata for key in ("width", "height", "pixel_format")):
            raise ValueError(f"raw image requires width, height, pixel_format in JSON metadata: {frame.path}")
        return packed_image(data, int(metadata["width"]), int(metadata["height"]), metadata["pixel_format"],
                            metadata.get("stride"), metadata.get("top_down", True))
    if data.startswith(b"BM"):
        return read_bmp(data)
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return read_png(data)
    if data.startswith((b"P3", b"P6")):
        return read_ppm(data)
    raise ValueError(f"unrecognized image format: {frame.path}")


def read_json(path):
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(document, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return document


def load_capture(folder, every=None):
    folder = Path(folder).resolve()
    if not folder.is_dir():
        raise ValueError(f"not a run/dump directory: {folder}")
    pattern = re.compile(r"frame[_-](\d+)\.(bmp|png|ppm|raw|rgb|rgba|bgra)$", re.I)
    groups = []
    for candidate in (folder, folder / "frames", folder / "dumps"):
        files = [(path, pattern.fullmatch(path.name)) for path in candidate.iterdir() if path.is_file()] if candidate.is_dir() else []
        files = [(path, int(match[1])) for path, match in files if match]
        if files:
            groups.append(files)
    if len(groups) != 1:
        raise ValueError(f"expected one frame directory (root, frames/, or dumps/), found {len(groups)} in {folder}")
    files = sorted(groups[0], key=lambda pair: (pair[1], pair[0].name))
    directory = files[0][0].parent
    record_path = folder / "run.json"
    if not record_path.is_file() and folder.name in ("frames", "dumps"):
        record_path = folder.parent / "run.json"
    record = read_json(record_path) if record_path.is_file() else {}
    settings = record.get("Settings", {})
    if not isinstance(settings, dict):
        raise ValueError(f"run Settings must be a JSON object: {record_path}")
    cadence = every if every is not None else int(settings.get("APS5_DUMP_FRAMES_EVERY", 1))
    if cadence < 1:
        raise ValueError("dump cadence must be positive")
    origin = datetime.fromisoformat(record["StartTime"]).timestamp() if record.get("StartTime") else min(path.stat().st_mtime for path, _ in files)
    common_path = directory / "metadata.json"
    common = read_json(common_path) if common_path.is_file() else {}
    frames = []
    for path, index in files:
        metadata = dict(common)
        for sidecar in (path.with_suffix(".json"), path.with_name(path.name + ".json")):
            if sidecar.is_file():
                metadata.update(read_json(sidecar))
        number = int(metadata.get("guest_frame", (index + 1) * cadence))
        timestamp = float(metadata.get("timestamp_s", path.stat().st_mtime - origin))
        if number < 0 or not math.isfinite(timestamp):
            raise ValueError(f"invalid frame number/timestamp: {path}")
        frames.append(Frame(path, number, timestamp, metadata))
    if len({frame.number for frame in frames}) != len(frames):
        raise ValueError(f"duplicate frame numbers in {directory}")
    inferred = sum("guest_frame" not in frame.metadata for frame in frames)
    file_times = sum("timestamp_s" not in frame.metadata for frame in frames)
    note = (f"{folder}: {len(frames)} frames; {inferred} numbers inferred as (dump index + 1) * {cadence}; "
            f"{file_times} timestamps from file mtime relative to {'run StartTime' if record.get('StartTime') else 'first dump'}.")
    if every is None and "APS5_DUMP_FRAMES_EVERY" not in settings and inferred:
        note += " Cadence unknown: assumed 1; supply --left-every/--right-every for sampled dumps."
    return sorted(frames, key=lambda frame: frame.number), note, record


def align_frames(left, right, mode, max_delta=1.0, right_offset=0.0):
    if mode == "frame":
        by_number = {frame.number: frame for frame in right}
        return [(frame, by_number[frame.number]) for frame in left if frame.number in by_number]
    ordered = sorted(right, key=lambda frame: (frame.timestamp, frame.number))
    times = [frame.timestamp + right_offset for frame in ordered]
    pairs = []
    for frame in left:
        position = bisect.bisect_left(times, frame.timestamp)
        candidates = range(max(0, position - 1), min(len(times), position + 1))
        nearest = min(candidates, key=lambda index: (abs(times[index] - frame.timestamp), index))
        if abs(times[nearest] - frame.timestamp) <= max_delta:
            pairs.append((frame, ordered[nearest]))
    return pairs


def metrics(left, right, threshold=10):
    if (left.width, left.height) != (right.width, right.height):
        raise ValueError(f"image dimensions differ: {left.width}x{left.height} vs {right.width}x{right.height}")
    if np is not None:
        delta = np.frombuffer(left.rgb, dtype=np.uint8).astype(np.int16) - np.frombuffer(right.rgb, dtype=np.uint8)
        mse = float(np.mean(delta.astype(np.float64) ** 2))
        fraction = float(np.mean(np.max(np.abs(delta.reshape(-1, 3)), axis=1) > threshold))
    else:
        squared, changed = 0, 0
        for offset in range(0, len(left.rgb), 3):
            differences = [abs(left.rgb[offset + channel] - right.rgb[offset + channel]) for channel in range(3)]
            squared += sum(value * value for value in differences)
            changed += max(differences) > threshold
        mse = squared / len(left.rgb)
        fraction = changed / (left.width * left.height)
    return math.inf if mse == 0 else 10 * math.log10(255 ** 2 / mse), fraction


def thumbnail(image, difference=None):
    scale = min(1.0, 320 / image.width, 180 / image.height)
    width, height = max(1, int(image.width * scale)), max(1, int(image.height * scale))
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            offset = ((y * image.height // height) * image.width + x * image.width // width) * 3
            pixel = image.rgb[offset:offset + 3]
            if difference is not None:
                level = max(abs(a - b) for a, b in zip(pixel, difference.rgb[offset:offset + 3])) * 4
                pixel = bytes((min(255, level), min(255, max(0, level - 255)), 0))
            pixels.extend(pixel)
    return Image(width, height, bytes(pixels))


def png_bytes(image):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    stride = image.width * 3
    raw = b"".join(b"\0" + image.rgb[start:start + stride] for start in range(0, len(image.rgb), stride))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def write_report(path, rows, notes, args, unmatched):
    failed = sum(row[2] < args.psnr_gate for row in rows)
    lines = ["<!doctype html><html lang='en'><meta charset='utf-8'><title>Frame comparison</title>",
             "<style>body{font:16px system-ui;margin:2rem}table{border-collapse:collapse}td,th{border:1px solid #aaa;padding:.4rem;text-align:left}img{max-width:100%;height:auto}.fail{background:#ffd6d6}</style>",
             f"<h1>Frame comparison: {'FAIL' if failed else 'PASS'}</h1>",
             f"<p>{len(rows)} aligned frames; {failed} below {args.psnr_gate:g} dB. "
             f"Unmatched: left {unmatched[0]}, right {unmatched[1]}. Alignment: {args.align}.</p>",
             f"<p>RGB PSNR; changed pixel = any RGB channel differs by more than {args.threshold}. Alpha ignored.</p>"]
    if args.align == "timestamp":
        lines.append(f"<p>Nearest elapsed timestamp, right offset {args.right_time_offset:g} s, "
                     f"maximum delta {args.max_time_delta:g} s. A right frame may serve multiple left frames.</p>")
    lines.extend(f"<p>{html.escape(note)}</p>" for note in notes)
    lines.append("<h2>Worst frames</h2><p>Heat-map: maximum absolute RGB difference, amplified 4x; black = equal, red/yellow = changed. Thumbnails use nearest pixels.</p>")
    for left, right, psnr, fraction in sorted(rows, key=lambda row: (row[2], -row[3]))[:args.worst]:
        a, b = read_image(left), read_image(right)
        lines.append(f"<h3>{html.escape(left.path.name)} / {html.escape(right.path.name)}: {psnr:.3f} dB, {fraction:.3%} changed</h3>")
        lines.append("<table><tr><th>Left</th><th>Right</th><th>Difference</th></tr><tr>")
        for label, picture in (("Left frame", thumbnail(a)), ("Right frame", thumbnail(b)), ("Difference heat-map", thumbnail(a, b))):
            encoded = base64.b64encode(png_bytes(picture)).decode("ascii")
            lines.append(f"<td><img alt='{label}' width='{picture.width}' height='{picture.height}' src='data:image/png;base64,{encoded}'></td>")
        lines.append("</tr></table>")
    lines.append("<h2>All aligned frames</h2><table><tr><th>Left file / frame</th><th>Right file / frame</th><th>Time delta (s)</th><th>PSNR (dB)</th><th>Changed pixels</th></tr>")
    for left, right, psnr, fraction in rows:
        delta = abs(left.timestamp - right.timestamp - args.right_time_offset)
        lines.append(f"<tr{' class=fail' if psnr < args.psnr_gate else ''}><td>{html.escape(left.path.name)} / {left.number}</td>"
                     f"<td>{html.escape(right.path.name)} / {right.number}</td><td>{delta:.6f}</td><td>{psnr:.6f}</td><td>{fraction:.6%}</td></tr>")
    lines.append("</table></html>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return failed


def main(argv=None):
    parser = argparse.ArgumentParser(description="Compare staged frame dumps; exit 0=pass, 1=PSNR gate failure, 2=input error.")
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--output", type=Path, default=Path("frame-compare.html"))
    parser.add_argument("--align", choices=("frame", "timestamp"), default="frame")
    parser.add_argument("--psnr-gate", type=float, default=30.0)
    parser.add_argument("--threshold", type=int, default=10, help="changed-pixel channel threshold, 0..255 (default 10)")
    parser.add_argument("--worst", type=int, default=5, help="number of visual comparisons (default 5)")
    parser.add_argument("--left-every", type=int, help="override left APS5_DUMP_FRAMES_EVERY")
    parser.add_argument("--right-every", type=int, help="override right APS5_DUMP_FRAMES_EVERY")
    parser.add_argument("--first-frame", type=int, help="inclusive left frame-number filter")
    parser.add_argument("--last-frame", type=int, help="inclusive left frame-number filter")
    parser.add_argument("--max-time-delta", type=float, default=1.0, help="timestamp matching tolerance in seconds (default 1)")
    parser.add_argument("--right-time-offset", type=float, default=0.0, help="seconds added to right timestamps")
    args = parser.parse_args(argv)
    if (not math.isfinite(args.psnr_gate) or args.psnr_gate < 0 or not 0 <= args.threshold <= 255 or args.worst < 0
            or not math.isfinite(args.max_time_delta) or args.max_time_delta < 0 or not math.isfinite(args.right_time_offset)):
        parser.error("gate/tolerance must be finite and nonnegative; threshold 0..255; worst nonnegative; offset finite")
    try:
        left, left_note, left_record = load_capture(args.left, args.left_every)
        right, right_note, right_record = load_capture(args.right, args.right_every)
        if left_record.get("Scene") and right_record.get("Scene") and left_record["Scene"] != right_record["Scene"]:
            raise ValueError("run.json Scene values differ; compare runs at the same scene")
        left = [frame for frame in left if (args.first_frame is None or frame.number >= args.first_frame)
                and (args.last_frame is None or frame.number <= args.last_frame)]
        pairs = align_frames(left, right, args.align, args.max_time_delta, args.right_time_offset)
        if not pairs:
            raise ValueError("no aligned frames; check cadence, frame range, or timestamp tolerance")
        rows = []
        for a, b in pairs:
            try:
                psnr, fraction = metrics(read_image(a), read_image(b), args.threshold)
            except (ValueError, struct.error, zlib.error) as error:
                raise ValueError(f"{a.path} / {b.path}: {error}") from error
            rows.append((a, b, psnr, fraction))
        unmatched = (len(left) - len(pairs), len(right) - len({b.path for _, b in pairs}))
        failed = write_report(args.output, rows, [left_note, right_note], args, unmatched)
        print(f"{len(rows)} aligned frames, {failed} below {args.psnr_gate:g} dB; report: {args.output}")
        return 1 if failed else 0
    except (OSError, ValueError, TypeError, KeyError, OverflowError, struct.error, zlib.error) as error:
        print(f"frame_compare: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
