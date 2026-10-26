# yolo_multi_demo

`yolo_multi_demo` is a multi-channel YOLO object detection demo that processes multiple input channels simultaneously.

## Overview

- Executable: `bin/yolo_multi_demo`
- Runs from configuration files
- Supports multi-channel object detection scenarios

At launch you can select one of the following configurations:

- `0`: `config/yolo_multi_demo.json`
- `1`: `config/ppu_yolo_multi_demo.json`
- `2`: `config/ppu_yolo_multi_100channel_demo.json`

## How to Run

```bash
cd yolo_multi_demo
./run_demo.sh
```

## What the Run Script Does

`run_demo.sh` handles the following automatically:

1. Runs `install.sh` if `bin/yolo_multi_demo` is missing.
2. Runs `setup.sh` if `assets/models` or `assets/videos` is missing.
3. Passes the configuration file selected from the menu via the `-c` option and starts the demo.

If no input is given, the default value `0` is selected.

## How to Exit

- Press `ESC` or `Q` while running to exit.
- Click the window's **`X`** (close) button to exit.
- Or press **`Ctrl+C`** in the console for a clean shutdown.

> **Windows note:** focusing the display window used to collapse inference
> throughput (~6×), because Windows dynamically boosts the foreground window's
> owning thread priority and it preempts the NPU worker threads. This is fixed
> at the source with `SetProcessPriorityBoost(GetCurrentProcess(), TRUE)` at
> startup (disables the boost process-wide), so the window is a normal focusable
> window and stays at full speed whether focused or not.

> **Windows note:** unless `display_config.output_width` / `output_height` are
> set explicitly, the composition buffer is fixed at FHD (1920x1080) instead of
> matching the monitor resolution. On a 4K display, compositing at the native
> resolution uses four times as many composition pixels; the actual CPU and
> temperature impact depends on the workload. The display window is still
> `WINDOW_NORMAL` (freely resizable /
> maximizable, and `is_fullsize_mode` still spans the whole monitor); the final
> upscale to whatever size the window ends up at is done by the OS-level blit
> in OpenCV's Win32 HighGUI backend, not by this demo's own `cv::resize` calls.

> **Crash fix:** a `"offline"` video source whose decoder fails to produce any
> frames during preload (e.g. an HEVC/H.265 clip with no decoder available,
> confirmed on Windows via Media Foundation) used to leave `_srcImg`/`_preImg`
> shorter than the configured preload count while playback still indexed them
> at the original length, corrupting memory and eventually crashing inside
> `VideoStream::ImgPostResize`. The preload step now clamps the usable frame
> count to what was actually captured. If none were captured, it logs an error,
> displays `INPUT ERROR`, and disables inference for that channel.

## Camera Expand Layout

When `video_sources` includes a camera input (one of `"camera"`, `"camera_image"`, `"camera_video"`), the first camera channel is automatically **expanded into the center of the grid** to emphasize it like a main window. The remaining channels are placed in the surrounding cells as usual.

- The expanded area size is automatically computed to stay within about 20% of the total grid area, and is progressively shrunk if there are not enough cells left for the other inputs (`scale ≤ floor(sqrt(0.20 × cols × rows))`).
- A **yellow border** is drawn around every cell containing a camera input for visual distinction.
- This does not apply when `display_config.expand_mode` (for special channel counts 33/41/61/73) is enabled.

## Additional display_config Options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `sidebar_font_scale` | float | 1.0 | Sidebar font scale |
| `fps_value_font_scale` | float | 0.5 | Top HUD FPS value font scale |
| `inference_fps` | integer | 15 | Maximum submissions per second per channel (1-1000); actual rate is also limited by capture and NPU completion. |
| `benchmark_mode` | boolean | false | Disable preload-result caching and the `inference_fps` cap; repeatedly infer at the `capture_period` cadence, still allowing only one in-flight request per channel. |
| `capture_period` | integer | 33 | Positive capture/playback period in milliseconds, **not FPS**. Existing configs commonly use 30 ms. |
| `display_fps` | integer | 30 | Main window refresh cap; independent of inference. Zero means unlimited. |

### Inference and playback

Normal mode continues capture/playback while inference is pending, without
queuing missed frames. It submits the current input when the rate limit and
the per-channel in-flight slot permit. An owned input snapshot remains valid
until the callback completes, even if capture advances. Camera/RTSP backend
buffers may still add latency; this is not a guarantee of zero capture latency.

For preloaded inputs (`"offline"`), detection results (including empty results)
are cached by stored-frame index. Each successfully loaded frame is inferred
at most once per run. Uncached frames use the latest available detections
until their own result arrives. Live/non-preloaded inputs do not use this cache.
The drawing toggle does not invalidate cached detections.

Display updates use a separate frame counter, so cached playback keeps moving
after inference stops. Console/HUD FPS counts **actual completed inferences**,
not cache hits or displayed frames; it therefore decays toward zero after
the cache warms up. Shutdown logs per-channel submitted/completed counts,
cache hits, cached frames, and produced display frames. Failed channels submit
zero requests in either mode.

For continuous throughput measurements, explicitly opt into benchmark mode:

```json
"display_config": {
    "capture_period": 30,
    "display_fps": 30,
    "inference_fps": 15,
    "benchmark_mode": true
}
```

For normal operation omit `benchmark_mode` or set it to `false`. Result caching
changes the meaning of repeated-video demonstrations; benchmark mode preserves
continuous inference but can significantly increase sustained NPU temperature.

Policy tests can be run without a DEEPX device or SDK:

```powershell
cmake -S tests -B build_x64\policy_tests
cmake --build build_x64\policy_tests --config Release
ctest --test-dir build_x64\policy_tests -C Release --output-on-failure
```

Example (part of `video_sources`):

```json
"video_sources": [
    [ "0", "camera" ],
    [ "./assets/videos/cctv-city-road.mov", "offline", 60 ],
    [ "./assets/videos/dance-group.mov",    "offline", 60 ]
]
```

## Stage Profiling (Linux vs Windows)

The demo can write a per-stage timing log so the same workload can be compared
between platforms. Nothing is measured unless profiling is turned on.

### Recording a log

```bash
# Linux
./run_demo.sh --profile                       # -> profile_linux_<N>ch_<timestamp>.log
./bin/yolo_multi_demo -c config/ppu_yolo_multi_640_demo.json --profile linux.log
```

```bat
REM Windows
yolo_multi_demo.exe -c config\ppu_yolo_multi_640_demo.json --profile
```

Run both sides for at least 60 seconds, then exit with `ESC` / `q` / the window
`X`. **The cumulative summary and the CSV block are written on exit**, so a
`kill -9` or a closed terminal leaves an incomplete log.

| Option | Default | Description |
|--------|---------|-------------|
| `--profile [path]` | off | Enable profiling. With no path, `profile_<os>_<N>ch_<timestamp>.log` is used. |
| `--profile_period <ms>` | 5000 | Interval-snapshot dump period |
| `--profile_warmup <ms>` | 3000 | Samples before this are discarded (model load, cache warm-up, first decode) |

It can also be enabled from the config json, in a top-level `profile` block
(the CLI options take precedence):

```json
"profile": {
    "enabled": true,
    "path": "",
    "period_ms": 5000,
    "warmup_ms": 3000,
    "per_channel": true
}
```

### What each stage means

Per-channel worker thread (`ObjectDetection::threadFunc`):

| Stage | Measures |
|-------|----------|
| `worker.loop` | Interval between loop iterations = this channel's actual frame period |
| `worker.busy` | Work per iteration, excluding sleep |
| `worker.get_input` | Capture/decode + letterbox resize (`VideoStream::GetInputStream`) |
| `worker.run_async` | NPU inference enqueue (`InferenceEngine::RunAsync`) |
| `worker.bbox` | bbox coordinate scaling (includes `_lock` wait) |
| `worker.get_output` | Source resize + bbox drawing (`GetOutputStream`) |
| `worker.badge` | `CH n` badge drawing |
| `worker.frame_swap` | Result frame swap (includes `_frameLock` wait) |
| `worker.sleep_req` / `worker.sleep_act` | Requested vs **actual** sleep time |

dxrt callback thread:

| Stage | Measures |
|-------|----------|
| `post.lock_wait` | Waiting for `_lock` |
| `post.yolo` | YOLO decode + NMS on the CPU |
| `post.gap` | Interval between callbacks = the channel's real inference rate (its FPS) |
| `npu.infer` / `npu.latency` | Values reported by dxrt |

Main render thread:

| Stage | Measures |
|-------|----------|
| `main.loop` / `main.busy` | Render loop period / work per iteration |
| `main.compose` | Copying each channel's result frame into the output board |
| `main.fps_calc` | FPS aggregation |
| `main.hud` | Header HUD rendering (Linux only) |
| `main.imshow` | `cv::imshow` |
| `main.waitkey` | `cv::waitKey(1)` |
| `main.winprop` | `cv::getWindowProperty` (window-closed check) |
| `main.sleep_req` / `main.sleep_act` | Requested vs actual sleep time |

The `cpu-ms/s` column is the total time a stage consumed per second summed over
all channels, so the largest number is the bottleneck.

The log header also records the NPU devices actually present (`dxrt` reports
them, so this is independent of the config's display-only `num_devices`), the
raw text of the config json, an estimate of how much memory the preload frame
buffers take, and the process/system memory state.

Every interval report carries a `[memory]` line:

```
[memory] rss 4475 MB | commit 9839 MB | peak_rss 4475 MB | sys_avail 23090/31965 MB | major_faults +18432 (3672/s)
```

A fault rate that stays high means the working set does not fit in physical
memory and pages are being re-read from disk — the usual cause of a stage whose
`avg` is many times its `p50`. On Linux the counter is major faults only (real
disk I/O); on Windows no per-process hard-fault counter exists, so it is the
total fault count (soft faults included) and only its *rate* is meaningful.

The header also records the OS, CPU, RAM, compiler, the full OpenCV build
information, and two probes that often explain a platform gap on their own:

- `clock.granularity_us` — resolution of `steady_clock`
- `sleep(1ms).actual_ms` — how long a 1 ms sleep really takes. Windows runs a
  15.6 ms default timer tick, which makes every `Sleep()` in the pipeline
  overshoot. Compare this against `sleep_req` vs `sleep_act` in the tables.

On Windows the demo raises the timer resolution to 1 ms for the lifetime of the
process (`timeBeginPeriod`), so `sleep(1ms).actual_ms` should read around 1 ms
and `timer.period_ms` in the header should read `1`. If the header instead says
the request failed, the pipeline is running on the 15.6 ms tick and every
`Sleep()` and `cv::waitKey(1)` is quantized to it. Since Windows 11 this
affects only the calling process.

### Comparing two logs

```bash
python3 scripts/compare_profile.py profile_linux_8ch_*.log profile_win_8ch_*.log
```

It prints the environment differences (mismatches marked `!!`), then a
stage table sorted by the largest absolute difference in total time, with a
`B/A` ratio per stage and `<<<` on anything differing by 30% or more.

## Screenshots

**0: Multi Channel Object Detection**

![yolo_multi_demo](img/yolo_multi_demo_screenshot.png)

**1: Multi Channel Object Detection With PPU**

![yolo_multi_demo_with_ppu](img/yolo_multi_demo_with_ppu_screenshot.png)

**2: Multi Channel 100ch Object Detection With PPU**

![yolo_multi_demo_100channel_with_ppu](img/yolo_multi_demo_100channel_with_ppu_screenshot.png)
