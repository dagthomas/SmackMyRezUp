# Render a mask preview movie: the ORIGINAL video with its segmentation masks
# drawn on top. No neural pass, no upscale - this is for judging the masks
# themselves before binding them into the neural pass.
#
#   python make_mask_preview.py <input.mp4>
#   python make_mask_preview.py <input.mp4> --mode matte --layers person,shield
#   python make_mask_preview.py <input.mp4> --mode split --out preview.mp4
#
# Layers are discovered beside the movie the same way the player finds them:
# every <stem>_mask_<phrase>.mp4 (one per GenMask prompt phrase), or the union
# <stem>_mask.mp4 when no per-phrase files exist. Each layer keeps the colour
# the player's Masks panel gives it, in the same order, so a preview reads the
# same as the live overlay: green, blue, yellow, magenta.
#
# Modes:
#   overlay  every layer tinted over the picture (default)
#   matte    the masked subject stays, everything else goes dark and grey -
#            the view for checking edges and spill
#   split    original on the left, overlay on the right, divider down the middle
#
# The mask videos are greyscale where white = process here, so a soft edge shows
# up as a soft tint. Nothing here is written back into the pipeline.
import argparse, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import ffmpeg_tools

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# The player's Masks panel colours (PlayerMain.cpp kLayerColor), in order.
LAYER_COLORS = [(51, 217, 115), (89, 140, 255), (255, 217, 64), (242, 89, 217)]
UNION_COLOR = (51, 217, 115)


def find_layers(src, wanted=None):
    """([(phrase, path)], all_phrases, missing) for this movie.

    Per-phrase files first, alphabetically by phrase; the union file alone when
    there are none. `wanted` filters and orders by the caller's list.

    `--layers` SELECTS among the sidecars that exist, it does not create them: a
    phrase only has a layer if GenMask was run with that phrase. Asking for one
    that was never generated is the easy mistake, so the names that did not
    match come back separately for the caller to report rather than being
    dropped in silence.
    """
    stem = os.path.splitext(src)[0]
    base = os.path.dirname(os.path.abspath(src)) or "."
    prefix = os.path.basename(stem) + "_mask_"
    found = []
    for name in sorted(os.listdir(base)):
        if not name.startswith(prefix) or not name.lower().endswith(".mp4"):
            continue
        found.append((name[len(prefix):-4], os.path.join(base, name)))
    if not found:
        union = stem + "_mask.mp4"
        if os.path.isfile(union):
            found = [("mask", union)]
    available = [p for p, _ in found]
    missing = []
    if wanted:
        # Phrases are slugged into file names ("drinking horn" -> drinking_horn),
        # so match either spelling.
        def key(s):
            return re.sub(r"[^a-z0-9]+", "_", s.strip().lower()).strip("_")

        order, seen = {}, set()
        for i, w in enumerate(wanted):
            k = key(w)
            if k:
                order.setdefault(k, i)
        for k in order:
            if not any(key(p) == k for p in available):
                missing.append(k)
        found = [f for f in found if key(f[0]) in order]
        found.sort(key=lambda f: order[key(f[0])])
    return found, available, missing


def probe(ffprobe, path):
    out = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height,r_frame_rate",
                          "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
    w, h, fr = out.stdout.strip().split(",")[:3]
    num, den = fr.split("/")
    return int(w), int(h), float(num) / float(den or 1)


def reader(ffmpeg, path, W, H, gray=False):
    """Raw frame pipe, rescaled to (W, H) so a mismatched sidecar still lines up."""
    fmt = "gray" if gray else "rgb24"
    return subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
         "-vf", f"scale={W}:{H}:flags=bilinear", "-f", "rawvideo", "-pix_fmt", fmt, "-"],
        stdout=subprocess.PIPE)


def load_font(size):
    for p in (r"C:\Windows\Fonts\segoeui.ttf", r"C:\Windows\Fonts\arial.ttf"):
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def draw_legend(frame, names, colors, scale):
    """Colour chip + phrase name per layer, top left, on a dark plate."""
    img = Image.fromarray(frame)
    d = ImageDraw.Draw(img, "RGBA")
    pad, chip, gap = int(14 * scale), int(15 * scale), int(9 * scale)
    font = load_font(max(11, int(18 * scale)))
    rows = [(n, c) for n, c in zip(names, colors)]
    if not rows:
        return np.asarray(img)
    widths = [d.textlength(n, font=font) for n, _ in rows]
    line = chip + gap + int(max(widths))
    box_h = pad * 2 + len(rows) * chip + (len(rows) - 1) * gap
    d.rectangle([pad, pad, pad * 2 + line, pad + box_h], fill=(0, 0, 0, 130))
    y = pad * 2
    for (name, color) in rows:
        d.rectangle([pad * 2, y, pad * 2 + chip, y + chip], fill=color + (255,))
        d.text((pad * 2 + chip + gap, y + chip // 2), name, font=font,
               fill=(240, 240, 240, 255), anchor="lm")
        y += chip + gap
    return np.asarray(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", default=None, help="default: <stem>_maskpreview.mp4")
    ap.add_argument("--mode", choices=("overlay", "matte", "split"), default="overlay")
    ap.add_argument("--layers", default=None,
                    help="comma-separated phrases to show, in order (default: all found)")
    ap.add_argument("--opacity", type=float, default=0.55, help="tint strength 0..1")
    ap.add_argument("--dim", type=float, default=0.82,
                    help="matte mode: how far the unmasked area is darkened 0..1")
    ap.add_argument("--no-legend", action="store_true")
    ap.add_argument("--crf", type=int, default=16)
    args = ap.parse_args()

    src = args.input
    if not os.path.isfile(src):
        sys.exit(f"[preview] no such file: {src}")
    out = args.out or os.path.splitext(src)[0] + "_maskpreview.mp4"
    ffmpeg, ffprobe = ffmpeg_tools()
    W, H, fps = probe(ffprobe, src)

    wanted = args.layers.split(",") if args.layers else None
    layers, available, missing = find_layers(src, wanted)
    if not available:
        sys.exit(f"[preview] no mask videos found beside {src}. "
                 f"Run make_mask_video.py first (GenMask in the player).")
    if missing:
        print(f"[preview] NOT FOUND: {', '.join(missing)} - no sidecar for "
              f"{'that phrase' if len(missing) == 1 else 'those phrases'}. "
              f"--layers selects among the masks that exist; to get one that is "
              f"missing, re-run make_mask_video.py with it in --prompt.", flush=True)
    if not layers:
        sys.exit(f"[preview] none of the requested layers exist. "
                 f"Available: {', '.join(available)}")
    print(f"[preview] available layers: {', '.join(available)}", flush=True)
    if len(layers) > len(LAYER_COLORS):
        dropped = [p for p, _ in layers[len(LAYER_COLORS):]]
        print(f"[preview] only {len(LAYER_COLORS)} colours exist; not showing: "
              f"{', '.join(dropped)}. Pick with --layers.", flush=True)
        layers = layers[:len(LAYER_COLORS)]
    colors = ([UNION_COLOR] if len(layers) == 1 and layers[0][0] == "mask"
              else LAYER_COLORS)[:len(layers)]

    print(f"[preview] {W}x{H} @ {fps:g} fps, mode={args.mode} -> {out}", flush=True)
    for (name, path), c in zip(layers, colors):
        print(f"[preview]   {name:12s} rgb{c}  {os.path.basename(path)}", flush=True)

    outW = W * 2 + 4 if args.mode == "split" else W
    dec = reader(ffmpeg, src, W, H)
    masks = [reader(ffmpeg, p, W, H, gray=True) for _, p in layers]
    enc = subprocess.Popen(
        [ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", f"{outW}x{H}",
         "-framerate", f"{fps}", "-i", "-", "-an",
         "-c:v", "libx264", "-preset", "medium", "-crf", str(args.crf),
         "-pix_fmt", "yuv420p", out], stdin=subprocess.PIPE)

    scale = max(0.6, min(2.0, H / 720.0))
    names = [n for n, _ in layers]
    fbytes, mbytes = W * H * 3, W * H
    n = 0
    try:
        while True:
            buf = dec.stdout.read(fbytes)
            if len(buf) < fbytes:
                break
            rgb = np.frombuffer(buf, np.uint8).reshape(H, W, 3).astype(np.float32)
            planes = []
            for m in masks:
                mb = m.stdout.read(mbytes)
                # A sidecar that runs short simply stops contributing.
                planes.append(np.frombuffer(mb, np.uint8).reshape(H, W).astype(np.float32) / 255.0
                              if len(mb) == mbytes else np.zeros((H, W), np.float32))

            if args.mode == "matte":
                keep = np.zeros((H, W), np.float32)
                for p in planes:
                    np.maximum(keep, p, out=keep)
                grey = rgb.mean(axis=2, keepdims=True)
                back = grey * (1.0 - args.dim) + rgb * 0.0
                comp = rgb * keep[..., None] + back * (1.0 - keep[..., None])
            else:
                comp = rgb.copy()
                for p, c in zip(planes, colors):
                    a = (p * args.opacity)[..., None]
                    comp = comp * (1.0 - a) + np.array(c, np.float32) * a

            comp = comp.clip(0, 255).astype(np.uint8)
            if not args.no_legend:
                comp = draw_legend(comp, names, colors, scale)

            if args.mode == "split":
                frame = np.zeros((H, outW, 3), np.uint8)
                frame[:, :W] = rgb.clip(0, 255).astype(np.uint8)
                frame[:, W:W + 4] = 240
                frame[:, W + 4:] = comp
            else:
                frame = comp
            enc.stdin.write(frame.tobytes())
            n += 1
            if n % 60 == 0:
                print(f"[preview] frame {n}", flush=True)
    finally:
        for p in [dec] + masks:
            if p.stdout:
                p.stdout.close()
        enc.stdin.close()
        enc.wait()
        for p in [dec] + masks:
            p.wait()
    print(f"[preview] done frames={n} -> {out}", flush=True)
    return 0 if n > 0 and enc.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
