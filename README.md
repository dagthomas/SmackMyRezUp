# SmackMyRezUp

A Windows x64 video player and exporter built around Direct3D 12, FFmpeg and the experimental NVIDIA **DLSS 5 Neural Rendering** runtime (NGX feature 18). It plays ordinary video files — and single images — through a game-style temporal pipeline: decoded frames are resized to the output resolution, given reconstructed motion-vector / depth / uncertainty guides, and redrawn by the neural pass at 1:1.

> **Status:** experimental. The neural runtime (`nvngx_dlssnr.dll`) is a pre-release NVIDIA component with an undocumented interface; behavior was mapped by measurement (see the comments in `src/NeuralEngine.h` and `src/ExportMain.cpp`). This project is not affiliated with or endorsed by NVIDIA.

## How the pipeline works

```text
video/image -> FFmpeg decode -> resize (bilinear / Lanczos3)
            -> guides: motion vectors (block matcher or RAFT flow), depth
               (heuristic proxy or Depth Anything V2), uncertainty mask
            -> DLSS-NR evaluate (colour + MVec bound; 1:1 at output res)
            -> tone preserve / sharpen / LUT -> display / export
```

Key measured facts baked into the defaults:

- The NR runtime consumes **`DLSSNR.MVec`** — binding it (zero vectors, scale −1) removes most temporal jitter while keeping the full neural effect. `DLSSNR.ControlMask` is off by default (guide bit `mask` binds it; white = process here, and merely binding it changes the output more than the mask's content does, so A/B against an all-white mask, never against unbound); `DLSSNR.Depth` is currently ignored by the runtime but stays wired for newer builds.
- The runtime exposes exactly one `ControlMask` and one set of `Intensity` / `LocalStructureStrength` / `LocalToneStrength` / `SkinStructureStrength` scalars per evaluate, plus `UseAutoMask` for the model's own character mask. NVIDIA's per-object "developer masking" (several named groups, each with its own structure and tone) is therefore an integration pattern on top of this interface (several evaluates, or a weight-valued mask), not extra inputs.
- Upscaling never uses DLSS Super Resolution: it is a deterministic resize followed by the neural detail pass, so `nvngx_dlss.dll` is not needed or shipped.

## Features

- **Player**: D3D12 renderer, FFmpeg decode (Media Foundation fallback), audio sync, seeking, looping, fullscreen, drag-and-drop, still-image import (PNG/JPG/WebP/BMP/TIFF/AVIF/JXL).
- **Neural controls**: model A/B/C, intensity, structure, skin, automask — all live per-frame; motion modes Zero / Global pan / Estimated (RAFT flow sidecar).
- **A/B tooling**: draggable vertical/horizontal split comparing the **pure original** against the full pipeline, with DLSS ON/OFF and FX ON/BYPASS labels; per-effect toggles and a Bypass-All key (B); debug views for the MV field, depth guide and uncertainty mask; inspection zoom.
- **Effects**: .cube LUT (right-click the LUT button to remove it), pre/post-sharpen, tone preserve, export-time NR-delta smoothing.
- **Sidecar generators** (Python with torch, see *Configuration*): `GenFlow` (RAFT optical flow), `GenDepth` (Depth Anything V2) and `GenMask` (text-prompted segmentation - SAM 3, falling back to Grounding DINO + SAM 2.1 when SAM 3's gated weights are not reachable: type `person. face. hair.`, get `<name>_mask.mp4` plus one `<name>_mask_<phrase>.mp4` per phrase); `<name>_flow.mp4` / `<name>_depth.mp4` / `<name>_mask.mp4` auto-attach to playback and exports. The mask is only *bound* into the neural pass while the **MaskNR** toggle is on (preview and export alike) — A/B against MaskNR off, never against a blank mask. Flow and depth take `--backend trt` to run through TensorRT (see *Optional: TensorRT*). Optional SeedVR restoration pre-pass (`<name>_svr.mp4`).
- **Export resolution**: the Res cycle offers preview size, 4K, 8K and **Native** — the source size with no upscale, so the neural pass redraws the original pixels 1:1 (the exporter auto-picks DLAA).
- **Exports**: Export / Compare / Split / 4K, resolution selector (preview / 4K / 8K long side), codec selector (x264, NVENC HEVC, NVENC AV1), processed-frame PNG snapshot, side-by-side comparison shots. Audio is muxed back from the source. Default names are `<name>_rezup.mp4`, `<name>_rezup_compare.mp4`, `<name>_rezup_4k.mp4`, `<name>_rezup_split.mp4` and `<name>_shot_N.png`.
- **UI**: collapsible, color-coded control groups with hover tooltips, uniform button grid, live-value sliders. English strings are compiled in.

## Headless exporter

`SmackMyRezUpExport.exe` is a console tool for batch/pipeline use (ComfyUI raw-pipe mode included):

```bat
SmackMyRezUpExport --input movie.mp4 --export out.mp4 --output-size 3840x2160 ^
    --tone preserve --tone-mix 0.7 --codec hevc --nr-structure 1.0
```

Notable flags: `--mv zero|global|estimated`, `--nr-guides off|on|all|mv,depth,mask`, `--nr-mvscale`, `--nr-smooth`, `--flow-video`, `--depth-video`, `--mask-video` (with `--nr-mask-mode bias|white|inv`), `--scaler lanczos`, `--codec x264|hevc|av1`, `--bits 16`, `--raw` (stdin/stdout frame piping), `--serve` (persistent batch server; it greets the host with `SMRU-SERVE 1`). Run with no arguments for the full list. Progress and diagnostics go to stderr as `[smru] ...` lines; the player's progress bar reads `[smru] frame N`.

## Building

```bat
build.bat
```

`build.ps1` clones the public NVIDIA/DLSS SDK (headers), stages FFmpeg, and requires the NR runtime: put `nvngx_dlssnr.dll` in `nr-runtime\` (or pass `-DlssnrDll <path>`). The finished `build\Release\SmackMyRezUp.exe` is **self-contained** — it embeds and self-extracts its runtime set on first launch:

```text
nvngx_dlssnr.dll        neural rendering runtime
ffmpeg.exe / ffprobe.exe
SmackMyRezUpExport.exe  headless exporter
fonts\                  UI typefaces
help.html               the manual (docs/help.html) - Help menu or F1 opens it
tools\                  the Python guide generators (GenFlow / GenDepth / GenMask / SeedVR)
luts\                   the sample .cube grades
```

Third-party files are never overwritten once extracted (swap in your own runtime or ffmpeg build freely), and so are the sample LUTs; the exporter, the fonts, the manual and the tools are replaced when a newer player is dropped beside an older install. The generators still need a Python with torch (see *Configuration*) — only the scripts ship.

The experimental runtime DLL itself is **not redistributable through this repository** — supply your own copy.

The version lives in one place, the `VERSION` file. CMake reads it into `project()`, generates `SmackMyRezUpVersion.h` from `src/Version.h.in`, and stamps a Windows VERSIONINFO block into both executables (`src/VersionInfo.rc`), so the title bar, the log and the file properties always agree. Bump `VERSION` to release.

## Configuration

Settings are stored in `SmackMyRezUp.ini` beside the player:

| Section | Contents |
| --- | --- |
| `[SmackMyRezUp]` | pipeline and UI state (effects, NR knobs, export presets, panel layout, LUT path). Values from the older `[DLSS]` section are read as a fallback and migrate on the next save. |
| `[VideoAdjustments]` | brightness, contrast, saturation, gamma, temperature, tint |
| `[General]` | `Language=` |
| `[Tools]` | `Python=` full path to a Python interpreter with torch, torchvision and transformers installed (ComfyUI's `python_embeded\python.exe` is the reference setup) |

Environment variables, all optional:

| Variable | Purpose |
| --- | --- |
| `SMRU_PYTHON` | interpreter for the sidecar generators; wins over `[Tools] Python=` |
| `SMRU_TOOLS_DIR` | folder holding the Python tools when they are not beside the exe or the checkout |
| `SMRU_COMFYUI_DIR` | ComfyUI portable root (SeedVR pack and weights live inside it) |
| `SMRU_BIN_DIR` | where the Python tools look for `ffmpeg.exe`, `ffprobe.exe` and the exporter; the player sets it for every child process |
| `SMRU_MODELS_DIR` | where the sidecar generators cache downloaded model weights; defaults to `models\` beside the exe |
| `SMRU_TENSORRT_DIR` | TensorRT SDK root or `bin` folder, when its DLLs are not on `PATH` |
| `SMRU_ENGINE_DIR` | where built TensorRT engines are cached (default `engines\` beside the checkout) |

Downloaded model weights land in `models\` **beside the exe** (`models\huggingface` for Depth Anything and SAM 3, `models\torch` for RAFT), not in the user profile, so copying an install to another machine brings its models with it. The player publishes `HF_HOME` and `TORCH_HOME` to its children to do that - it leaves them alone if you already set either one, and falls back to the per-user default when the app folder is not writable (an install under `Program Files`). SeedVR2 is the exception: its weights stay inside the ComfyUI install that runs it.

Nothing in the code depends on where the checkout lives: `tools\` and `luts\` are found beside the exe or by walking up from `build\Release\`.

## Optional: TensorRT

`make_depth_video.py --backend trt` and `make_flow_video.py --backend trt` run the same models through TensorRT instead of PyTorch. Measured on an RTX 5090: Depth Anything V2 Small 5.3 → 1.3 ms per frame, RAFT-small 15 → 5–7 ms, with sidecars that match the torch backend (depth to 0.05 of 255 levels on average; flow to 0.07 px). Needs the CUDA Toolkit, the TensorRT SDK (`bin` on `PATH` or `SMRU_TENSORRT_DIR`) and its Python wheel installed into the tools interpreter (`pip install --no-deps <TensorRT>\python\tensorrt-*-cp3XX-none-win_amd64.whl`), plus `onnx` and `onnxscript` for the export.

Engines are compiled per GPU, TensorRT version, input size and precision, so nothing prebuilt ships with the project: the first run at a new resolution exports ONNX and builds (5–30 s), then the engine is reused from `engines\`. Every RAFT engine is measured against the torch model before use, because TensorRT's kernel search is not reproducible and some of its choices break RAFT badly (0.4 px average, 80 px at the border); an engine that fails is discarded and rebuilt at optimization level 0, which is deterministic. See `tools\trt_runtime.py`.

## Optional: ReShade / RenoDX

The ReShade + RenoDX DLSS 5 add-on path still works for overlay experiments: install ReShade (with add-on support, DX10/11/12) for `SmackMyRezUp.exe` and place the add-on files beside the exe. The player keeps presenting while paused so the ReShade UI stays responsive, and global hotkeys (Ctrl+Alt+Space etc.) work while ReShade captures input.

## Project layout

```text
src/
  PlayerMain.cpp          GUI player entry point (PlayerApp)
  ExportMain.cpp          headless exporter entry point
  AppIdentity.h           every product name, file name, window class, env var,
                          suffix and log tag - the single source of identity
  AppPaths.h              run-time discovery of tools/, luts/ and Python
  Version.h.in            -> build/generated/SmackMyRezUpVersion.h (from VERSION)
  SmackMyRezUp.rc         player resources: VERSIONINFO + embedded payload
  SmackMyRezUpExport.rc   exporter resources: VERSIONINFO
  VersionInfo.rc          shared VERSIONINFO block (#included by the two above)
  VideoDecoder.*          FFmpeg pipe / Media Foundation decode
  TemporalGuides.*        CPU motion / depth-proxy / uncertainty guides
  D3D12Renderer.*         D3D12 pipeline, effects, present and export readback
  NeuralEngine.*          host for the DLSS-NR runtime (NGX feature 18)
  DLSSBackend.*           renderer-facing adapter over NeuralEngine
  AudioPlayer.*, Localization.h, CubeLUT.h, LabelStamp.h, Log.h
tools/
  smru_env.py             shared helper: finds ffmpeg/ffprobe/exporter/ComfyUI
  make_flow_video.py      RAFT optical-flow sidecar   (<name>_flow.mp4)
  make_depth_video.py     Depth Anything V2 sidecar   (<name>_depth.mp4)
  make_mask_video.py      Text-prompted segmentation sidecar, SAM 3 or SAM 2.1 (<name>_mask.mp4)
  trt_runtime.py          TensorRT: locate the SDK, build/cache/verify engines, run them
  run_seedvr.py           SeedVR2 restoration pre-pass (<name>_svr.mp4)
  depth_probe.py          checks whether the runtime consumes depth
engines/                  TensorRT engine cache, machine-specific (git-ignored)
payload/                  files embedded into the player (see src/SmackMyRezUp.rc)
luts/                     sample .cube grades
nr-runtime/               drop folder for nvngx_dlssnr.dll (see its README)
CMakeLists.txt            SmackMyRezUpCore static lib + the two executables
build.ps1 / build.bat     one-command local Windows build
run_4k_auto.bat, run_4k_quality.bat   launch presets
VERSION                   MAJOR.MINOR.PATCH, the only place the version is typed
```

## License

The project source code is licensed under the **MIT License**. See [LICENSE](LICENSE).

Third-party components such as NVIDIA DLSS/NGX, FFmpeg, ReShade, RenoDX, RAFT, Depth Anything and SeedVR are separate projects and remain subject to their own licenses and terms. Experimental DLSS 5 runtime files are not distributed by this repository.

DLSS and NVIDIA are trademarks of NVIDIA Corporation.
