# Generate a Depth Anything V2 depth-map video for a movie.
#
#   python make_depth_video.py <input.mp4> [--out <path>] [--size 518]
#   python make_depth_video.py <input.mp4> --backend trt      (TensorRT)
#
# Depth Anything V2 Small produces relative inverse depth per frame; the 2-98
# percentile range is normalised with a temporal EMA so the mapping stays stable
# between adjacent frames, and the depth itself is blended with the previous
# frame (scene cuts reset both). Encoded as grayscale video, BRIGHT = NEAR - the
# convention SmackMyRezUp and SmackMyRezUpExport --depth-video expect (they
# invert it into the 0 = near R16 NGX contract).
# Written as <stem>_depth.mp4.
#
# Two backends produce the same sidecar. --backend torch runs the HuggingFace
# model directly; --backend trt compiles it into a TensorRT engine on first use
# (a few minutes, cached under engines/) and runs that instead. Preprocessing is
# identical either way - resize and normalise happen on the GPU in torch - so
# the only difference between the two is inference arithmetic.
import argparse, os, subprocess, sys
# ComfyUI's embedded Python does not put the script folder on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import ensure_torch, ffmpeg_tools

ensure_torch()   # hand over to an interpreter with torch when this one lacks it

import numpy as np
import torch

MODEL_ID = "depth-anything/Depth-Anything-V2-Small-hf"
PATCH = 14                   # DINOv2 patch size: every input side is a multiple
SCENE_CUT_THRESHOLD = 0.20   # mean abs gray diff that counts as a cut
DEPTH_HISTORY = 0.25         # EMA weight of the previous depth frame
RANGE_HISTORY = 0.95         # EMA weight of the previous 2-98% range
# The image processor shipped with the model: rescale to [0,1], then ImageNet
# statistics. Kept here so both backends share one preprocessing path.
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)


class _DepthGraph(torch.nn.Module):
    """predicted_depth only - ONNX export cannot return the HF output object."""

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, pixel_values):
        return self.model(pixel_values=pixel_values).predicted_depth


def _load_hf(dtype, dev):
    from transformers import AutoModelForDepthEstimation
    return AutoModelForDepthEstimation.from_pretrained(MODEL_ID, dtype=dtype).to(dev).eval()


def export_onnx(path, fp16, ih, iw):
    """Write the depth model to ONNX at one fixed input shape."""
    dtype = torch.float16 if fp16 else torch.float32
    model = _load_hf(dtype, "cuda")
    dummy = torch.zeros(1, 3, ih, iw, dtype=dtype, device="cuda")
    torch.onnx.export(_DepthGraph(model).eval(), (dummy,), str(path),
                      input_names=["pixel_values"], output_names=["predicted_depth"],
                      opset_version=18, dynamo=True)


class TorchBackend:
    def __init__(self, ih, iw, dev, dtype):
        self.dtype = dtype
        print("[depthgen] loading Depth Anything V2 Small (first run downloads ~100 MB)...",
              flush=True)
        self.model = _load_hf(dtype, dev)

    def __call__(self, pixel_values):
        with torch.inference_mode():
            return self.model(pixel_values=pixel_values.to(self.dtype)).predicted_depth


class TrtBackend:
    """One engine per (model, input shape, precision) - see tools/trt_runtime.py."""

    def __init__(self, ih, iw, fp16):
        import trt_runtime
        self.engine = trt_runtime.load_or_build(
            "depth-anything-v2-small", (1, 3, ih, iw),
            lambda path, half: export_onnx(path, half, ih, iw),
            fp16=fp16, log=lambda m: print(f"[depthgen] {m}", flush=True))

    def __call__(self, pixel_values):
        return self.engine(pixel_values=pixel_values)["predicted_depth"]


def infer_size(W, H, size):
    """The (height, width) the model's own image processor would resize to.

    DPT keeps the aspect ratio and rounds both sides to a multiple of the 14 px
    patch, fitting the long side to `size`. Feeding a square instead - which is
    what a naive size={"height":n,"width":n} produces on paper but not in
    practice - stretches the frame and visibly degrades the depth.
    """
    scale = size / max(W, H)
    to14 = lambda v: max(PATCH, int(round(v * scale / PATCH)) * PATCH)
    return to14(H), to14(W)


def preprocess(rgb, ih, iw, dev):
    """HWC uint8 frame -> normalised NCHW float32 at the inference size."""
    t = torch.from_numpy(rgb.copy()).to(dev)
    t = t.permute(2, 0, 1).unsqueeze(0).float().div_(255.0)
    # antialias matches PIL's bicubic, which is what the model's own image
    # processor uses; without it a downscale aliases and the depth crawls.
    t = torch.nn.functional.interpolate(t, size=(ih, iw), mode="bicubic",
                                        align_corners=False, antialias=True)
    mean = torch.tensor(IMAGENET_MEAN, device=dev).view(1, 3, 1, 1)
    std = torch.tensor(IMAGENET_STD, device=dev).view(1, 3, 1, 1)
    return (t - mean) / std


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", default=None)
    ap.add_argument("--size", type=int, default=518,
                    help="inference size for the long side (aspect ratio is kept)")
    ap.add_argument("--backend", choices=("torch", "trt"), default="torch",
                    help="trt compiles a TensorRT engine on first use (cached in engines/)")
    ap.add_argument("--fp32", action="store_true",
                    help="full precision inference (default is fp16 on the GPU)")
    args = ap.parse_args()

    src = args.input
    out = args.out or os.path.splitext(src)[0] + "_depth.mp4"
    ffmpeg, ffprobe = ffmpeg_tools()

    probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height,r_frame_rate",
                            "-of", "csv=p=0", src], capture_output=True, text=True, check=True)
    w, h, fr = probe.stdout.strip().split(",")[:3]
    W, H = int(w), int(h)
    num, den = fr.split("/")
    fps = float(num) / float(den or 1)
    print(f"[depthgen] {W}x{H} @ {fps:g} fps -> {out}", flush=True)

    dev = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = not args.fp32 and dev.type == "cuda"
    ih, iw = infer_size(W, H, args.size)
    if args.backend == "trt":
        if dev.type != "cuda":
            sys.exit("--backend trt needs a CUDA device")
        model = TrtBackend(ih, iw, fp16)
    else:
        model = TorchBackend(ih, iw, dev, torch.float16 if fp16 else torch.float32)
    print(f"[depthgen] backend={args.backend} precision={'fp16' if fp16 else 'fp32'} "
          f"inference={iw}x{ih}", flush=True)

    dec = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", src,
                            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                           stdout=subprocess.PIPE)
    enc = subprocess.Popen([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                            "-f", "rawvideo", "-pix_fmt", "gray", "-video_size", f"{W}x{H}",
                            "-framerate", f"{fps}", "-i", "-",
                            "-c:v", "libx264", "-preset", "fast", "-crf", "6",
                            "-pix_fmt", "yuv420p", out],
                           stdin=subprocess.PIPE)

    def estimate(rgb):
        pred = model(preprocess(rgb, ih, iw, dev))
        if pred.dim() == 3:
            pred = pred.unsqueeze(1)
        with torch.inference_mode():
            pred = torch.nn.functional.interpolate(pred.float(), size=(H, W), mode="bicubic",
                                                   align_corners=False).squeeze()
        return pred.cpu().numpy().astype(np.float32)

    frame_bytes = W * H * 3
    prev_gray = None
    prev_depth = None
    depth_low = depth_high = None
    n = 0
    while True:
        buf = dec.stdout.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        rgb = np.frombuffer(buf, np.uint8).reshape(H, W, 3)
        gray = rgb.mean(axis=2).astype(np.float32)
        reset = n == 0
        if prev_gray is not None:
            reset = float(np.abs(gray - prev_gray).mean()) / 255.0 >= SCENE_CUT_THRESHOLD

        raw = estimate(rgb)
        lo, hi = np.percentile(raw, (2.0, 98.0))
        if reset or depth_low is None:
            depth_low, depth_high = float(lo), float(hi)
        else:
            depth_low = depth_low * RANGE_HISTORY + float(lo) * (1.0 - RANGE_HISTORY)
            depth_high = depth_high * RANGE_HISTORY + float(hi) * (1.0 - RANGE_HISTORY)
        if depth_high <= depth_low:
            depth = np.zeros_like(raw)
        else:
            # Depth Anything produces relative inverse depth; a stable
            # normalisation keeps adjacent frames comparable. Bright = near.
            depth = np.clip((raw - depth_low) / (depth_high - depth_low), 0.0, 1.0)
        if prev_depth is not None and not reset:
            depth = prev_depth * DEPTH_HISTORY + depth * (1.0 - DEPTH_HISTORY)
        prev_depth = depth
        prev_gray = gray

        enc.stdin.write((depth * 255.0 + 0.5).astype(np.uint8).tobytes())
        n += 1
        if n % 30 == 0:
            print(f"[depthgen] frame {n}", flush=True)

    dec.stdout.close()
    enc.stdin.close()
    enc.wait()
    dec.wait()
    print(f"[depthgen] done frames={n} -> {out}", flush=True)
    return 0 if n > 0 and enc.returncode == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
