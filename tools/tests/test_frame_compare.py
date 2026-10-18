import contextlib
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import frame_compare as compare


def bmp(width, height, rgb, bits=32, top_down=True):
    stride = ((width * bits + 31) // 32) * 4
    pixels = bytearray()
    for y in range(height) if top_down else reversed(range(height)):
        row = bytearray()
        for x in range(width):
            offset = (y * width + x) * 3
            row.extend(reversed(rgb[offset:offset + 3]))
            if bits == 32:
                row.append(0)
        pixels.extend(row + bytes(stride - len(row)))
    return (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
            + struct.pack("<IiiHHIIiiII", 40, width, -height if top_down else height, 1, bits, 0, len(pixels), 2835, 2835, 0, 0)
            + pixels)


class FrameCompareTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.left, self.right = self.root / "250", self.root / "251"
        self.left.mkdir()
        self.right.mkdir()
        self.black = compare.Image(2, 1, bytes(6))
        self.changed = compare.Image(2, 1, bytes((255, 0, 0, 0, 0, 0)))

    def write(self, folder, index, image=None):
        image = image or self.black
        path = folder / f"frame_{index:03d}.bmp"
        path.write_bytes(bmp(image.width, image.height, image.rgb))
        return path

    def run_cli(self, *arguments):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return compare.main([str(self.left), str(self.right), "--output", str(self.root / "report.html"), *arguments])

    def test_metrics_exact_psnr_and_strict_pixel_threshold_with_both_backends(self):
        expected = 10 * math.log10(6)
        for backend in (None, compare.np):
            with self.subTest(numpy=backend is not None), mock.patch.object(compare, "np", backend):
                self.assertEqual(compare.metrics(self.black, self.black), (math.inf, 0.0))
                psnr, fraction = compare.metrics(self.black, self.changed)
                self.assertAlmostEqual(psnr, expected)
                self.assertEqual(fraction, 0.5)
                self.assertEqual(compare.metrics(self.black, self.changed, 255)[1], 0.0)

    def test_driver_bmp_and_padded_bottom_up_bmp(self):
        rgb = bytes((10, 20, 30, 40, 50, 60))
        for bits, top_down in ((32, True), (24, False)):
            with self.subTest(bits=bits):
                self.assertEqual(compare.read_bmp(bmp(1, 2, rgb, bits, top_down)), compare.Image(1, 2, rgb))

    def test_cli_exit_codes_and_embedded_heatmap(self):
        self.write(self.left, 0)
        self.write(self.right, 0, self.changed)
        self.assertEqual(self.run_cli(), 1)
        report = (self.root / "report.html").read_text(encoding="utf-8")
        self.assertIn("Frame comparison: FAIL", report)
        self.assertIn("50.000000%", report)
        self.assertEqual(report.count("data:image/png;base64,"), 3)
        self.assertIn("Difference heat-map", report)
        self.assertEqual(self.run_cli("--psnr-gate", "7"), 0)
        self.write(self.right, 0)
        self.assertEqual(self.run_cli(), 0)
        process = subprocess.run([sys.executable, "-B", str(Path(compare.__file__)), str(self.left), str(self.right),
                                  "--output", str(self.root / "subprocess.html")], capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.write(self.right, 0, self.changed)
        process = subprocess.run([sys.executable, "-B", str(Path(compare.__file__)), str(self.left), str(self.right),
                                  "--output", str(self.root / "subprocess.html")], capture_output=True, text=True)
        self.assertEqual(process.returncode, 1, process.stderr)

    def test_run_folder_settings_align_sampled_presentation_numbers(self):
        for folder, cadence, switches in ((self.left, 5, "off"), (self.right, 10, "on")):
            (folder / "frames").mkdir()
            (folder / "run.json").write_text(json.dumps({"Scene": "level", "PerfSwitches": switches,
                                                        "Settings": {"APS5_DUMP_FRAMES_EVERY": str(cadence)}}), encoding="utf-8-sig")
        self.write(self.left / "frames", 0, self.changed)
        self.write(self.left / "frames", 1)
        self.write(self.right / "frames", 0)
        self.assertEqual(self.run_cli(), 0)
        report = (self.root / "report.html").read_text(encoding="utf-8")
        self.assertIn("frame_001.bmp / 10", report)
        self.assertIn("Unmatched: left 1, right 0", report)
        self.assertEqual(self.run_cli("--right-every", "5"), 1)
        self.assertEqual(self.run_cli("--first-frame", "11"), 2)

    def test_nearest_elapsed_timestamps_tolerance_and_offset(self):
        for folder, origin, indices in ((self.left, 1000, (0, 1)), (self.right, 9000, (7, 8))):
            for elapsed, index in enumerate(indices):
                path = self.write(folder, index)
                os.utime(path, (origin + elapsed * 2, origin + elapsed * 2))
        self.assertEqual(self.run_cli(), 2)
        self.assertEqual(self.run_cli("--align", "timestamp", "--max-time-delta", "0"), 0)
        self.assertEqual(self.run_cli("--align", "timestamp", "--right-time-offset", "10"), 2)
        for folder, start in ((self.left, "1970-01-01T00:16:35+00:00"), (self.right, "1970-01-01T02:29:55+00:00")):
            (folder / "run.json").write_text(json.dumps({"StartTime": start}), encoding="utf-8")
        self.assertEqual(self.run_cli("--align", "timestamp", "--max-time-delta", "0"), 0)
        a, _, _ = compare.load_capture(self.left)
        b, _, _ = compare.load_capture(self.right)
        self.assertEqual(a[0].timestamp, 5.0)
        b[1].timestamp = 7.2
        self.assertEqual(len(compare.align_frames(a, b, "timestamp", 0.1)), 1)
        self.assertEqual(len(compare.align_frames(a, b, "timestamp", 0.3)), 2)

    def test_raw_sidecar_dimensions_and_guest_frame_override(self):
        self.write(self.left, 0, self.changed)
        (self.left / "frame_000.json").write_text(json.dumps({"guest_frame": 42, "timestamp_s": 7.0}), encoding="utf-8")
        raw = self.right / "frame_007.raw"
        raw.write_bytes(bytes((0, 0, 255, 200, 0, 0, 0, 255)))
        (self.right / "metadata.json").write_text(json.dumps({"width": 2, "height": 1, "pixel_format": "BGRA8"}), encoding="utf-8")
        sidecar = raw.with_name(raw.name + ".json")
        sidecar.write_text(json.dumps({"guest_frame": 42, "timestamp_s": 7.1}), encoding="utf-8")
        frames, _, _ = compare.load_capture(self.right)
        self.assertEqual(compare.read_image(frames[0]), self.changed)
        self.assertEqual(self.run_cli(), 0)
        self.assertEqual(self.run_cli("--align", "timestamp", "--max-time-delta", "0.2"), 0)
        self.assertEqual(self.run_cli("--align", "timestamp", "--max-time-delta", "0.01"), 2)
        (self.right / "metadata.json").unlink()
        self.assertEqual(self.run_cli(), 2)
        raw.write_bytes(b"BM\0\0\0\0")
        sidecar.write_text(json.dumps({"width": 2, "height": 1, "pixel_format": "RGB"}), encoding="utf-8")
        frames, _, _ = compare.load_capture(self.right)
        self.assertEqual(compare.read_image(frames[0]).rgb, b"BM\0\0\0\0")

    def test_ppm_binary_first_pixel_whitespace_and_ascii_scaling(self):
        rgb = bytes((10, 13, 32, 255, 0, 1))
        self.assertEqual(compare.read_ppm(b"P6\n# dimensions\n2 1\n255\n" + rgb), compare.Image(2, 1, rgb))
        self.assertEqual(compare.read_ppm(b"P6\r\n2 1\r\n255\r\n" + rgb), compare.Image(2, 1, rgb))
        self.assertEqual(compare.read_ppm(b"P3\n1 1\n15\n15 0 3\n"), compare.Image(1, 1, bytes((255, 0, 51))))

    def test_png_filters_and_report_thumbnail_round_trip(self):
        rows = [bytes((15 + y, 25 + y, 35 + y, 45 + y, 55 + y, 65 + y)) for y in range(5)]
        filtered = bytearray()
        previous = bytes(6)
        for method, row in enumerate(rows):
            filtered.append(method)
            for x, value in enumerate(row):
                a, b, c = row[x - 3] if x >= 3 else 0, previous[x], previous[x - 3] if x >= 3 else 0
                predictor = (0, a, b, (a + b) // 2, compare.paeth(a, b, c))[method]
                filtered.append((value - predictor) & 255)
            previous = row
        encoded = compare.png_bytes(compare.Image(2, 5, b"".join(rows)))
        start = encoded.index(b"IDAT") - 4
        end = start + 12 + struct.unpack_from(">I", encoded, start)[0]
        compressed = zlib.compress(filtered)
        idat = struct.pack(">I", len(compressed)) + b"IDAT" + compressed + struct.pack(">I", zlib.crc32(b"IDAT" + compressed))
        encoded = encoded[:start] + idat + encoded[end:]
        self.assertEqual(compare.read_png(encoded), compare.Image(2, 5, b"".join(rows)))
        self.assertEqual(compare.read_png(compare.png_bytes(self.changed)), self.changed)
        heatmap = compare.thumbnail(self.black, self.changed)
        self.assertEqual(heatmap.rgb, bytes((255, 255, 0, 0, 0, 0)))

    def test_input_errors_do_not_pass_the_gate(self):
        self.assertEqual(self.run_cli(), 2)
        self.write(self.left, 0)
        path = self.write(self.right, 0)
        path.write_bytes(b"BMbroken")
        self.assertEqual(self.run_cli(), 2)
        self.write(self.right, 0, compare.Image(1, 1, bytes(3)))
        self.assertEqual(self.run_cli(), 2)
        self.write(self.right, 0)
        (self.right / "frame_000.ppm").write_bytes(b"P6\n2 1\n255\n" + bytes(6))
        self.assertEqual(self.run_cli(), 2)


if __name__ == "__main__":
    unittest.main()
