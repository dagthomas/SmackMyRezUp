# Generate a RAFT optical-flow video for a movie.
#
#   python make_flow_video.py <input.mp4> [--size 1024] [--out <path>]
#   python make_flow_video.py <input.mp4> --backend trt        (TensorRT)
#
# For each frame t, computes flow t -> t-1 (current pixel's displacement to its
# previous-frame location - the DLSS motion-vector convention) with
# torchvision's RAFT-small on the GPU, and encodes it as a 10-bit video:
#   R = dx, G = dy, mapped from [-range .. +range] source pixels across the
#   full code range (mid-code = zero motion), B = mid-grey. First frame is
#   zero flow. The range (--range, default 64 px) is written into the file as
#   the smru_flow_range metadata tag, which the player and exporter read back.
# Written as <stem>_flow.mp4 (yuv444p10le, x264 -qp 4): 10 bits over
# +/-64 px is a 0.15 px step, finer than the old 8-bit +/-24 px file (0.19 px)
# with 2.7x the reach - a handheld pan at 720p passes 24 px/frame easily, and
# a clamped vector is a wrong vector. The quantiser is fixed rather than
# rate-controlled: measured on a field with sharp motion boundaries, crf 6
# leaves 0.1% of the vectors more than 1 px off (ringing up to 12 px at the
# edges of moving objects, which is where the neural pass needs them most);
# -qp 4 keeps every vector within 0.75 px at ~2.3x the file size, and -qp 0
# (lossless) would cost 5.7x. SmackMyRezUp and SmackMyRezUpExport decode this
# and feed it as the per-pixel MV field, replacing the noisy CPU block matcher.
#
# The default inference size is a 1024 px long side: measured on 720p handheld
# footage the 1024 field keeps the neural output more stable along the motion
# than 512 (warp error 4.43 vs 4.56 on a 4.15 floor) at the same wall time,
# which is dominated by the decode and encode rather than RAFT.
#
# --backend trt compiles RAFT into a TensorRT engine on first use (cached under
# engines/, keyed by inference size, GPU and TensorRT version) and runs that
# instead. Everything around it is unchanged, so the two backends write the
# same sidecar. RAFT's iterative refinement is unrolled into the engine, so an
# engine is tied to --iters as well as to the size. Both backends run in fp32:
# torchvision's RAFT allocates float32 tensors inside forward(), so a half
# model neither runs nor exports, and the correlation volume is precision
# sensitive anyway.
import argparse, os, subprocess, sys
# ComfyUI's embedded Python does not put the script folder on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import ensure_torch, ffmpeg_tools

ensure_torch()   # hand over to an interpreter with torch when this one lacks it

import numpy as np
import torch
import torch.nn.functional as F
from torchvision.models.optical_flow import raft_small, Raft_Small_Weights

FLOW_RANGE = 64.0  # +/- source pixels encoded across the code range (--range)
ITERS = 12         # RAFT refinement steps; torchvision's own default
# How far a built engine may sit from the torch model on the synthetic pair in
# verify_pair(), in inference pixels. Calibrated against measured builds: a
# sound autotuned engine scores 0.018, the deterministic level-0 fallback
# 0.054, and a broken engine 0.6-0.8 (which is 0.4 px mean and 80 px max on
# real footage). The limit sits between the clusters with margin both ways.
TOLERANCE = 0.10


class _FlowGraph(torch.nn.Module):
    """The last refinement step only - ONNX cannot return RAFT's list."""

    def __init__(self, model, iters):
        super().__init__()
        self.model = model
        self.iters = iters

    def forward(self, image1, image2):
        return self.model(image1, image2, num_flow_updates=self.iters)[-1]


def _load_raft(dev):
    return raft_small(weights=Raft_Small_Weights.DEFAULT).to(dev).eval()


def export_onnx(path, ih, iw, iters):
    """Write RAFT to ONNX at one fixed input shape and iteration count."""
    graph = _FlowGraph(_load_raft("cuda"), iters).eval()
    dummy = torch.zeros(1, 3, ih, iw, device="cuda")
    torch.onnx.export(graph, (dummy, dummy.clone()), str(path),
                      input_names=["image1", "image2"], output_names=["flow"],
                      opset_version=18, dynamo=True)


class TorchBackend:
    def __init__(self, ih, iw, dev, iters):
        self.iters = iters
        print("[flowgen] loading RAFT-small (first run downloads ~4 MB)...", flush=True)
        self.model = _load_raft(dev)

    def __call__(self, image1, image2):
        with torch.inference_mode():
            return self.model(image1, image2, num_flow_updates=self.iters)[-1]


def verify_pair(ih, iw, dev):
    """A synthetic frame pair: one texture, two windows a few pixels apart.

    Smooth noise rendered slightly larger than the frame, then cropped twice,
    so the pair is a clean translation with real content entering at two
    edges - where a badly built engine goes wrong first (measured: torch never
    exceeded 13 px of flow on real footage while a bad engine reached 77 px,
    all of it starting at the top border). Cropping rather than rolling
    matters: a wrapped seam is an ill-posed flow problem on which even two
    correct implementations disagree by half a pixel.
    """
    m, dy, dx = 8, 3, 5
    g = torch.Generator().manual_seed(7)
    small = torch.rand(1, 3, (ih + 2 * m) // 8 + 1, (iw + 2 * m) // 8 + 1, generator=g)
    big = F.interpolate(small, size=(ih + 2 * m, iw + 2 * m), mode="bicubic",
                        align_corners=False).clamp(0.0, 1.0) * 2.0 - 1.0
    a = big[:, :, m:m + ih, m:m + iw]
    b = big[:, :, m - dy:m - dy + ih, m - dx:m - dx + iw]
    return a.contiguous().to(dev), b.contiguous().to(dev)


class TrtBackend:
    """One engine per (input shape, iteration count, precision, GPU).

    TensorRT's kernel choice is timing-based and not reproducible, and some
    choices break RAFT badly (see tools/trt_runtime.py), so every engine is
    checked against the torch model before it is used.
    """

    def __init__(self, ih, iw, iters, reference):
        import trt_runtime
        a, b = verify_pair(ih, iw, "cuda")
        ref = reference(a, b).float().reshape(1, 2, ih, iw)

        def verify(engine):
            got = engine(image1=a, image2=b)["flow"].float().reshape(1, 2, ih, iw)
            return float((got - ref).abs().mean())

        self.engine = trt_runtime.load_or_build(
            f"raft-small-i{iters}", (1, 3, ih, iw),
            lambda path, _fp16: export_onnx(path, ih, iw, iters),
            fp16=False, verify=verify, tolerance=TOLERANCE,
            log=lambda m: print(f"[flowgen] {m}", flush=True))

    def __call__(self, image1, image2):
        return self.engine(image1=image1, image2=image2)["flow"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--size", type=int, default=1024, help="inference long side")
    ap.add_argument("--out", default=None)
    ap.add_argument("--backend", choices=("torch", "trt"), default="torch",
                    help="trt compiles a TensorRT engine on first use (cached in engines/)")
    ap.add_argument("--iters", type=int, default=ITERS,
                    help="RAFT refinement steps; fewer is faster and blurrier")
    ap.add_argument("--range", type=float, default=FLOW_RANGE,
                    help="+/- source px the encoding spans; written as the smru_flow_range tag")
    args = ap.parse_args()
    rng = args.range
    if not rng > 0.0:
        sys.exit("--range must be positive")

    src = args.input
    out = args.out or os.path.splitext(src)[0] + "_flow.mp4"
    ffmpeg, ffprobe = ffmpeg_tools()

    probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height,r_frame_rate",
                            "-of", "csv=p=0", src], capture_output=True, text=True, check=True)
    w, h, fr = probe.stdout.strip().split(",")[:3]
    W, H = int(w), int(h)
    num, den = fr.split("/")
    fps = float(num) / float(den or 1)
    print(f"[flowgen] {W}x{H} @ {fps:g} fps -> {out}", flush=True)

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    scale = args.size / max(W, H)
    iw = max(64, int(W * scale) // 8 * 8)
    ih = max(64, int(H * scale) // 8 * 8)
    up_x, up_y = W / iw, H / ih

    if args.backend == "trt":
        if dev != "cuda":
            sys.exit("--backend trt needs a CUDA device")
        # The torch model is loaded either way: the engine is verified against it.
        model = TrtBackend(ih, iw, args.iters, TorchBackend(ih, iw, dev, args.iters))
    else:
        model = TorchBackend(ih, iw, dev, args.iters)
    print(f"[flowgen] backend={args.backend} inference={iw}x{ih} iters={args.iters} range=+/-{rng:g} px (10-bit)", flush=True)

    dec = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", src,
                            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                           stdout=subprocess.PIPE)
    # 16-bit RGB in, 10-bit 4:4:4 H.264 out. The range tag needs use_metadata_tags,
    # or the MP4 muxer drops keys it does not know.
    enc = subprocess.Popen([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                            "-f", "rawvideo", "-pix_fmt", "rgb48le", "-video_size", f"{W}x{H}",
                            "-framerate", f"{fps}", "-i", "-",
                            "-c:v", "libx264", "-preset", "fast", "-qp", "4",
                            "-pix_fmt", "yuv444p10le",
                            "-movflags", "use_metadata_tags", "-metadata", f"smru_flow_range={rng:g}",
                            out],
                           stdin=subprocess.PIPE)

    def to_tensor(buf):
        a = np.frombuffer(buf, np.uint8).reshape(H, W, 3).copy()
        t = torch.from_numpy(a).permute(2, 0, 1).float().unsqueeze(0).to(dev)
        t = F.interpolate(t, size=(ih, iw), mode="bilinear", align_corners=False)
        return t / 127.5 - 1.0

    frame_bytes = W * H * 3
    zero_enc = np.full((H, W, 3), 32768, np.uint16)
    prev = None
    n = 0
    with torch.no_grad():
        while True:
            buf = dec.stdout.read(frame_bytes)
            if len(buf) < frame_bytes:
                break
            cur = to_tensor(buf)
            if prev is None:
                enc.stdin.write(zero_enc.tobytes())
            else:
                flow = model(cur, prev)                     # t -> t-1, inference px
                flow = F.interpolate(flow.float().reshape(1, 2, ih, iw), size=(H, W),
                                     mode="bilinear", align_corners=False)[0]
                flow[0] *= up_x
                flow[1] *= up_y
                f = flow.permute(1, 2, 0).clamp(-rng, rng).cpu().numpy()
                img = np.empty((H, W, 3), np.uint16)
                img[..., 0] = np.clip(np.rint(f[..., 0] / (2 * rng) * 65535.0 + 32767.5), 0, 65535)
                img[..., 1] = np.clip(np.rint(f[..., 1] / (2 * rng) * 65535.0 + 32767.5), 0, 65535)
                img[..., 2] = 32768
                enc.stdin.write(img.tobytes())
            prev = cur
            n += 1
            if n % 30 == 0:
                print(f"[flowgen] frame {n}", flush=True)

    dec.stdout.close()
    enc.stdin.close()
    enc.wait()
    dec.wait()
    print(f"[flowgen] done frames={n} -> {out}", flush=True)
    return 0 if n > 0 and enc.returncode == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
