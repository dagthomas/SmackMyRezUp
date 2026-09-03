# Security

## Scope

SmackMyRezUp is a local desktop application. It has no accounts, no telemetry and no network listener. The player and the exporter never open a network connection themselves; the only network activity happens on the explicit user actions listed below.

## What runs and loads with your rights

- **Self-extracting payload.** On first launch `SmackMyRezUp.exe` writes the files embedded at build time beside itself: `nvngx_dlssnr.dll`, `ffmpeg.exe`, `ffprobe.exe`, `SmackMyRezUpExport.exe` and `fonts\`. Existing files are never overwritten, so a file already beside the exe wins over the embedded copy. Keep the player in a folder that untrusted users cannot write to: anything placed there is loaded or executed as you.
- **Helper processes.** The player launches `ffmpeg.exe`, `ffprobe.exe` and `SmackMyRezUpExport.exe` from its own folder, and the Python tools in `tools\` (`make_flow_video.py`, `make_depth_video.py`, `run_seedvr.py`) through a user-configured interpreter.
- **Configuration that selects executables.** `SmackMyRezUp.ini` (`[Tools] Python=`) and the environment variables `SMRU_PYTHON`, `SMRU_TOOLS_DIR`, `SMRU_BIN_DIR` and `SMRU_COMFYUI_DIR` decide which interpreter, scripts and binaries are used. Treat them like `PATH`: whoever can change them can run code as you.
- **Third-party graphics add-ons.** ReShade and RenoDX DLLs placed beside the exe are loaded into the process if you install them. They, the experimental DLSS 5 runtime and Streamline files are outside this repository's trust boundary. Only use runtime files from sources you trust.

## Network activity

- `build.ps1` clones `github.com/NVIDIA/DLSS` and downloads an FFmpeg build from `gyan.dev` over HTTPS.
- The SeedVR restoration (`tools\run_seedvr.py`) downloads model weights from Hugging Face through the SeedVR2 ComfyUI pack, which checks them against pinned SHA-256 hashes before they are used.
- The depth and flow generators download their models (Depth Anything V2, RAFT) through `transformers` and `torchvision` on first use.

## Inputs

Media files are decoded by FFmpeg, or by Media Foundation as a fallback; the usual caution about opening untrusted media applies. `.cube` LUTs and `.lang` files are read by small self-contained parsers in `src\`. The exporter's `--serve` mode reads commands only from its own standard input.

## Logs

`SmackMyRezUp.log` and `SmackMyRezUpExport.log` contain file paths and tool output. Review them before sharing.

## Reporting

For security-sensitive reports that should not be public, contact the repository maintainer privately rather than posting exploit details in a public issue.
