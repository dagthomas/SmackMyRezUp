# SeedVR2 restoration pre-pass for the SmackMyRezUp pipeline.
#
#   python run_seedvr.py <input.mp4> [--resolution N] [--out <path>]
#   python run_seedvr.py --check [--model 7b_sharp]     -> "ready" or "missing: ..."
#
# Runs the SeedVR2 diffusion restorer (via the seedvr2_videoupscaler pack's own
# CLI) at the source's short side by default - pure restoration, no upscale -
# writing <stem>_svr.mp4 beside the input. The restored file is then played /
# exported through DLSS+NR, which amplifies the recovered detail.
#
# Weights live in ComfyUI's models\SEEDVR2 (7B sharp fp8 ~8.5 GB + the 0.5 GB
# VAE). They are downloaded and verified HERE, as a separate phase before any
# inference starts: the pack's own download runs inside inference_cli with a
# 30 s socket timeout and two retries, so a stalled first attempt used to kill
# the whole run and only the next click resumed the partial file. Every phase
# is announced on a "[seedvr] ..." line so the player can show it.
#
# Default recipe follows VRGDG SeedVR2 TensorRT Studio: the 7B *sharp* DiT +
# SageAttention 2. Their last exclusive (TensorRT VAE engines) is approximated
# by --compile (torch.compile of DiT+VAE; opt-in - first run pays compile time).
# If the 7B model OOMs at every batch size, the ladder drops to the 3B fp8.
import argparse, os, subprocess, sys, time
# ComfyUI's embedded Python does not put the script folder on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import comfyui_root, ffmpeg_tools

# Console encoding: the pack prints emoji while importing, which raises under a
# cp1252 console or pipe. Speak UTF-8 on our streams and the child's (the player
# decodes the progress pipe as UTF-8).
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")
CHILD_ENV = {**os.environ, "PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1"}

MODELS = {
    "7b_sharp": "seedvr2_ema_7b_sharp_fp8_e4m3fn_mixed_block35_fp16.safetensors",
    "7b_sharp_fp16": "seedvr2_ema_7b_sharp_fp16.safetensors",
    "7b": "seedvr2_ema_7b_fp8_e4m3fn_mixed_block35_fp16.safetensors",
    "3b": "seedvr2_ema_3b_fp8_e4m3fn.safetensors",
    "3b_fp16": "seedvr2_ema_3b_fp16.safetensors",
}
VAE_FILE = "ema_vae_fp16.safetensors"     # the pack's DEFAULT_VAE
DOWNLOAD_ATTEMPTS = 40                    # each attempt resumes the partial file

# The SeedVR2 pack and its weights live inside the ComfyUI portable install
# whose python_embeded interpreter runs this script (or SMRU_COMFYUI_DIR).
COMFY = comfyui_root()
if COMFY is None:
    sys.exit("ComfyUI portable root not found: run with its python_embeded\\python.exe or set SMRU_COMFYUI_DIR")
PACK = str(COMFY / "ComfyUI" / "custom_nodes" / "seedvr2_videoupscaler")
MODEL_DIR = str(COMFY / "ComfyUI" / "models" / "SEEDVR2")


def say(message: str) -> None:
    print(f"[seedvr] {message}", flush=True)


def missing_weights(model_key: str) -> list[str]:
    """Weight files for *model_key* that are not present in MODEL_DIR yet."""
    return [name for name in (MODELS[model_key], VAE_FILE)
            if not os.path.isfile(os.path.join(MODEL_DIR, name))]


class _DownloadLog:
    """Minimal stand-in for the pack's debug logger: forwards its messages."""

    def log(self, message, **_kwargs):
        say(str(message))


def ensure_weights(model_key: str) -> bool:
    """Download and verify the DiT + VAE weights for *model_key* before inference.

    Uses the pack's own resumable downloader, but drives it with patient
    retries so a stalled connection never fails the restoration itself.
    """
    dit = MODELS[model_key]
    missing = missing_weights(model_key)
    if missing:
        say(f"downloading model {', '.join(missing)} -> {MODEL_DIR} "
            f"(first run; the 7B DiT is ~8.5 GB, this takes a while)")
    else:
        say(f"validating model {dit}")
    sys.path.insert(0, PACK)   # the pack imports itself as "src.*"
    from src.utils.downloads import download_weight
    from src.utils.model_registry import DEFAULT_VAE
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        if download_weight(dit_model=dit, vae_model=DEFAULT_VAE, model_dir=MODEL_DIR, debug=_DownloadLog()):
            say(f"model ready {dit}")
            return True
        wait = min(30, 3 * attempt)
        say(f"downloading model attempt {attempt}/{DOWNLOAD_ATTEMPTS} did not finish; resuming in {wait}s")
        time.sleep(wait)
    say("FAILED: could not download the model - check network and disk space")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("--check", action="store_true",
                    help="report whether the weights for --model are present and exit "
                         "(0 = ready, 3 = missing); no download, no inference")
    ap.add_argument("--resolution", type=int, default=0,
                    help="short-side target. 0 = auto: upscale to 1080 (capped at 2x source), "
                         "never below source")
    ap.add_argument("--strength", type=float, default=0.7,
                    help="0..1 blend of the restoration over the (upscaled) original; "
                         "1 = full SeedVR look, lower = gentler")
    ap.add_argument("--out", default=None)
    ap.add_argument("--load_cap", type=int, default=0, help="limit frames (testing)")
    ap.add_argument("--batch", type=int, default=33,
                    help="temporal batch (4n+1!). Bigger = far more temporally stable; "
                         "the tiny default of 5 makes the restoration pump every 5 frames.")
    ap.add_argument("--model", choices=sorted(MODELS), default="7b_sharp",
                    help="DiT model. 7b_sharp (default) = the VRGDG-studio pick; "
                         "3b = the old lighter default")
    ap.add_argument("--attention", default="sageattn_2",
                    choices=["sdpa", "flash_attn_2", "flash_attn_3", "sageattn_2", "sageattn_3"],
                    help="attention kernel (sageattn_2 is installed and fastest here)")
    ap.add_argument("--compile", action="store_true",
                    help="torch.compile the DiT+VAE (our stand-in for VRGDG's TensorRT "
                         "engines; first run pays warm-up compile time)")
    args = ap.parse_args()

    if args.check:
        missing = missing_weights(args.model)
        print("ready" if not missing else "missing: " + ", ".join(missing), flush=True)
        return 0 if not missing else 3
    if not args.input:
        ap.error("input is required unless --check is given")

    ffmpeg, ffprobe = ffmpeg_tools()
    src = os.path.abspath(args.input)
    out = args.out or os.path.splitext(src)[0] + "_svr.mp4"
    probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height,r_frame_rate", "-of", "csv=p=0", src],
                           capture_output=True, text=True, check=True)
    parts = probe.stdout.strip().split(",")
    w, h = int(parts[0]), int(parts[1])
    num, den = parts[2].split("/")
    fps = float(num) / float(den or 1)
    short = min(w, h)
    res = args.resolution
    if res <= 0:
        res = min(max(short, 1080), short * 2)   # upscale toward 1080p, never past 2x
    strength = min(max(args.strength, 0.0), 1.0)
    os.makedirs(MODEL_DIR, exist_ok=True)
    say(f"{src} -> {out} (short side {short}->{res}, strength {strength:g}, "
        f"{fps:g} fps, model {args.model})")

    raw = os.path.splitext(out)[0] + "_raw.mp4"
    # Temporal batch = stability, but memory scales with pixels x batch. Scale the
    # batch to the output pixel budget (33 was validated at ~1.0 MP) and snap to
    # the required 4n+1; enable VAE tiling for big frames.
    out_mp = (res * (res * max(w, h) / short)) / 1e6
    target = int(round(args.batch * min(1.0, 1.05 / max(out_mp, 0.1))))
    target = max(9, min(args.batch, (max(target - 1, 8) // 4) * 4 + 1))
    tiled = out_mp > 1.5
    say(f"output ~{out_mp:.1f} MP -> batch {target}{' + VAE tiling' if tiled else ''}")
    ladder = sorted({target, 17, 9}, reverse=True)
    ladder = [b for b in ladder if b <= target] or [9]
    # If the chosen (7B) model OOMs at every batch size, retry the whole ladder
    # on the lighter 3B before giving up.
    model_ladder = [args.model] + (["3b"] if args.model not in ("3b", "3b_fp16") else [])
    ok = False
    for model in model_ladder:
        if not ensure_weights(model):
            continue
        for batch in ladder:
            overlap = 4 if batch > 5 else 0
            say(f"restoring: trying model={model} batch_size={batch} temporal_overlap={overlap} "
                f"attention={args.attention}")
            cmd = [sys.executable, os.path.join(PACK, "inference_cli.py"), src,
                   "--output", raw, "--resolution", str(res),
                   "--batch_size", str(batch), "--temporal_overlap", str(overlap),
                   "--uniform_batch_size", "--model_dir", MODEL_DIR,
                   "--dit_model", MODELS[model],
                   "--attention_mode", args.attention]
            if args.compile:
                cmd += ["--compile_dit", "--compile_vae"]
            if tiled:
                cmd += ["--vae_encode_tiled", "--vae_decode_tiled"]
            if args.load_cap > 0:
                cmd += ["--load_cap", str(args.load_cap)]
            if subprocess.call(cmd, cwd=PACK, env=CHILD_ENV) == 0 and os.path.exists(raw):
                ok = True
                break
            say(f"model {model} batch {batch} failed; retrying smaller")
        if ok:
            break
    if not ok:
        say("FAILED at all model/batch sizes")
        return 1

    # Finalize (always): enforce the source frame rate, mux the ORIGINAL audio
    # back in (the SeedVR writer is video-only), tag BT.709, and at strength < 1
    # composite the restoration over the lanczos-upscaled original. scale2ref
    # guarantees the blend inputs match SeedVR's actual output size exactly.
    say(f"finalizing (fps {fps:g}, audio mux, strength {strength:g})")
    if strength < 0.999:
        fc = (f"[0:v][1:v]scale2ref=flags=lanczos[o][r];"
              f"[o][r]blend=all_mode=normal:all_opacity={strength},fps={fps},"
              f"setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1[v]")
    else:
        fc = (f"[1:v]fps={fps},"
              f"setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1[v]")
    rc = subprocess.call([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
        "-i", src, "-i", raw,
        "-filter_complex", fc,
        "-map", "[v]", "-map", "0:a:0?", "-c:a", "aac",
        "-c:v", "libx264", "-preset", "medium", "-crf", "12", "-pix_fmt", "yuv420p",
        "-shortest", out])
    try: os.remove(raw)
    except OSError: pass
    if rc != 0 or not os.path.exists(out):
        say("finalize failed")
        return 1
    say(f"done -> {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
