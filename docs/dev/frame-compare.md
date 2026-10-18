# Comparing staged frame dumps

Run `python tools/frame_compare.py <left-run-or-dump-directory> <right-run-or-dump-directory>`
with Python 3.10 or newer. It uses the standard library and uses numpy for metrics
if numpy is already importable. Nothing needs installing. Invoking this standalone
tool opts into comparison; it adds no driver switch or runtime behavior.

For example, preserve captures from run 250 (switch group off) and run 251
(switch group on), both at the same scene, under a directory outside the shared
build tree:

```text
captures/250/run.json
captures/250/frames/frame_000.bmp
captures/250/frames/frame_001.bmp
captures/251/run.json
captures/251/frames/frame_000.bmp
captures/251/frames/frame_001.bmp
```

Copy each run's `run.json` and its completed frames into its capture directory,
preserving file modification times if timestamp matching will be used. The tool
accepts images directly in each input directory, or in one `frames/` or `dumps/`
child. It rejects ambiguous directories containing multiple image sets.

```powershell
python tools/frame_compare.py captures/250 captures/251 --output reports/250-vs-251.html
python tools/frame_compare.py captures/250/frames captures/251/frames --first-frame 600 --last-frame 900 --psnr-gate 32 --threshold 12 --worst 8 --output reports/250-vs-251-scene.html
```

The HTML is self-contained: all aligned pairs have PSNR and changed-pixel values,
and the worst five pairs have embedded side-by-side thumbnails and heat-maps.
Exit codes are **0** for all aligned pairs meeting the gate, **1** if any pair is
below the gate, and **2** for invalid input, incompatible dimensions, or no matches.
An input error prints a diagnostic; it does not produce a new report.

## What the driver writes

[`VulkanDevice.cpp`](../../core/libs/prx/libSceAgcDriver/Execution/src/VulkanDevice.cpp)
contains `Dumps()`, `PresentDisplayBuffer()`, and `WriteFrameBmp()`.
The staged launcher is
`build/demons-souls-runtime/diagnosis/performance-fixes/launch-claude-followup.ps1`.
It starts the game with working directory `build/demons-souls-control-ce3432`.
The driver writes `frame_%03d.bmp` there: uncompressed 32-bit **BGRA**, top-down
(negative BMP height), with a 54-byte header. Dimensions are in the BMP header;
there is no per-frame metadata sidecar. `APS5_DUMP_SCALE=N` keeps every Nth pixel
on each axis; dimensions are rounded up. Both runs should use the same scale.

`APS5_DUMP_FRAMES` limits the number of dumps (launcher: 3000).
`APS5_DUMP_FRAMES_EVERY=N` samples every Nth eligible display-buffer presentation,
starting at presentation N. The filename index starts at **zero** and increments
per dump, so `frame_001.bmp` with N=5 corresponds to eligible presentation **10**.
This is an inferred presentation number, not an independently recorded guest frame
ID: linear-buffer presents bypass this counter, and dropped GPU presents can alter
the correspondence. `PresentPixels()`, used by tests/tools, instead dumps the first
N calls at full size and ignores the cadence/scale settings.

The launcher records effective settings in `run.json` under `Settings`. Before a
new run it moves existing root-level BMPs to `frames-before-<new run>`; that archive
belongs to the **previous** run. It creates `runs/<run>/run.json`, but does not put
the frame dumps there. Preserve a capture set deliberately rather than treating
`frames-before-251` as run 251 or recursively mixing runtime archives.

## Alignment and metrics

Default `--align frame` matches equal frame numbers. `run.json` is read from the
input run directory or the parent of a `frames/` or `dumps/` input. Without explicit
frame metadata, numbers are `(dump index + 1) * APS5_DUMP_FRAMES_EVERY`. Bare dumps
without a run record assume cadence 1 and report that assumption; supply
`--left-every 5 --right-every 5` when needed. `--first-frame` and `--last-frame`
select an inclusive range of left frame numbers, useful for excluding menus.

`--align timestamp` matches each left frame to the nearest right elapsed timestamp
within `--max-time-delta` seconds (default 1). A right frame may be reused. Without
sidecars, file modification time is measured relative to `run.json`'s `StartTime`,
or the earliest dump if no start time exists. These are dump-completion times,
not guest timestamps. `--right-time-offset SECONDS` adds a scene-alignment offset
to right timestamps. Prefer number matching when testing changes that alter speed.
The report shows unmatched counts; unmatched frames do not affect the PSNR gate.

PSNR is `10 * log10(255² / MSE)` over all RGB channel samples; identical RGB is
infinite PSNR. The default gate is **30 dB**. A pixel differs when **any** RGB channel
has absolute difference strictly greater than `--threshold` (default **10**, range
0–255). Alpha is ignored. Heat-maps show maximum RGB error amplified 4x, from black
through red to yellow. Metrics use full dump resolution; thumbnails sample nearest
pixels and may miss isolated changes. Images are never resized for metrics.

## Other image inputs and tests

Decoders also accept non-interlaced 8-bit PNG (RGB/RGBA/grayscale/paletted), 24-bit
uncompressed BMP, and P3/P6 PPM with maxima 1–255. Unsupported encodings fail
explicitly. Files must be named `frame_<integer>.<extension>` (hyphen also accepted).
Raw `.raw`, `.rgb`, `.rgba`, and `.bgra` need JSON metadata; no dimensions are guessed:

```json
{"width": 960, "height": 540, "pixel_format": "BGRA8", "guest_frame": 600, "timestamp_s": 12.5}
```

Use `frame_000.raw.json` or `frame_000.json` beside the image. `metadata.json` supplies
directory defaults; image sidecars override them. Raw `pixel_format` accepts RGB,
BGR, RGBA, BGRA, optionally suffixed `8`; optional `stride` is bytes per row and
`top_down` defaults to true. For every format, `guest_frame` overrides the inferred
number and `timestamp_s` supplies elapsed seconds. Keep a consistent timestamp
origin across each capture.

Run `python tools/tests/test_frame_compare.py`. CTest registration is `frame_compare`;
the unit suite checks synthetic metrics, pass/fail process exits, switch-off/on run
records, cadence and timestamp alignment, decoder variants, and invalid input.
