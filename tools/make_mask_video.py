# Generate a text-prompted segmentation mask video for a movie.
#
#   python make_mask_video.py <input.mp4> --prompt "person. face."
#   python make_mask_video.py <input.mp4> --prompt "human" --out mask.mp4
#   python make_mask_video.py <input.mp4> --prompt "pitcher. grapes." --layers
#
# Two backends, chosen with --model (default auto: SAM 3 when it is reachable,
# Grounding DINO + SAM 2.1 otherwise):
#
#  * SAM 3 has its own text encoder, so a phrase goes straight to pixel-accurate
#    instance masks in one pass. Its weights are GATED on Hugging Face - request
#    access at https://huggingface.co/facebook/sam3 and authenticate
#    (`hf auth login`, or set HF_TOKEN) - so it is not available to everyone.
#  * SAM 2.1 has no text encoder: it is prompted with points, boxes or masks,
#    never words. There the prompt is resolved in two stages - Grounding DINO
#    turns the text into boxes, SAM 2.1 turns the boxes into masks. Nothing is
#    gated, which is why it stays as the fallback.
#
# Either way prompt phrases are short noun phrases, lower case, separated by
# periods or commas ("person. face." rather than "person and face") - the format
# Grounding DINO was trained on, and one concept per phrase for SAM 3.
#
# The result is a grayscale movie where WHITE = process here, which is the
# semantic the NR runtime's DLSSNR.ControlMask was measured to use. Written as
# <stem>_mask.mp4, which the player attaches automatically and the exporter
# takes via --mask-video. Neither binds it unless the mask guide bit is set
# (--nr-guides mask, or the player's MaskNR toggle).
#
# --layers additionally writes one movie per phrase, <stem>_mask_<phrase>.mp4,
# so each object can later get its own structure/tone treatment the way
# NVIDIA's developer masking groups do. The union is still written as before.
#
# Masks are re-detected every --detect-every frames, carried unchanged in
# between, and blended with the previous frame so a subject that flickers in and
# out of the model does not strobe the mask.
import argparse, os, re, subprocess, sys
# ComfyUI's embedded Python does not put the script folder on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import ffmpeg_tools

import numpy as np
import torch
from PIL import Image

# SAM 3 ships a single checkpoint (~1.6 GB on the first run); sensitivity is a
# score threshold rather than a bigger network. The SAM 2.1 fallback does have a
# size ladder, and defaults to the largest pair because it is an offline job
# where quality beats the roughly 3x time; --dino tiny --sam small is the fast
# end (0.8 GB of weights instead of 1.7 GB).
SAM3_ID = "facebook/sam3"
DINO_IDS = {"tiny": "IDEA-Research/grounding-dino-tiny",
            "base": "IDEA-Research/grounding-dino-base"}
SAM_IDS = {"small": "facebook/sam2.1-hiera-small",
           "base": "facebook/sam2.1-hiera-base-plus",
           "large": "facebook/sam2.1-hiera-large"}
SCENE_CUT_THRESHOLD = 0.20   # mean abs gray diff that counts as a cut
MASK_HISTORY = 0.35          # EMA weight of the previous mask frame

GATED_HELP = (
    "SAM 3's weights are gated. Request access at https://huggingface.co/facebook/sam3, "
    "then sign in with `hf auth login` or set HF_TOKEN."
)


def normalize_prompt(text):
    """Split the prompt into the short lower-case noun phrases both models expect."""
    parts = [p.strip().lower() for chunk in text.split(".") for p in chunk.split(",")]
    parts = [p for p in parts if p]
    if not parts:
        sys.exit("--prompt is empty: try --prompt \"person. face.\"")
    return " ".join(p + "." for p in parts), parts


def phrase_slug(phrase):
    """File-name-safe form of a phrase: 'red car' -> 'red_car'."""
    return re.sub(r"[^a-z0-9]+", "_", phrase.lower()).strip("_") or "phrase"


def union_masks(masks, H, W):
    """Any stack of instance masks -> one (H,W) plane in [0,1]. Empty -> zeros."""
    if masks is None:
        return np.zeros((H, W), np.float32)
    if hasattr(masks, "detach"):
        masks = masks.detach().to("cpu")
    m = np.asarray(masks, dtype=np.float32)
    while m.ndim > 3:     # (N,1,H,W) for several instances
        m = m[:, 0]
    if m.ndim == 2:       # (H,W) for exactly one
        m = m[None]
    if m.ndim != 3 or len(m) == 0:
        return np.zeros((H, W), np.float32)
    return np.clip(m.max(axis=0), 0.0, 1.0)


def looks_gated(error_text):
    """Is this load failure the Hugging Face gate rather than a real fault?"""
    return any(k in error_text for k in ("401", "403", "gated", "Gated", "awaiting", "authorized"))


def sam3_detector(dev, args, phrases, H, W):
    """SAM 3: text -> instance masks in one pass. Raises when it cannot load."""
    # Named classes, not AutoProcessor/AutoModel: facebook/sam3 declares the
    # VIDEO processor and model in its config, so the Auto classes hand back
    # Sam3VideoProcessor/Sam3VideoModel, which take an inference session and
    # reject `text=`. The per-image pair is what this frame-at-a-time loop wants.
    from transformers import Sam3Model, Sam3Processor
    processor = Sam3Processor.from_pretrained(SAM3_ID)
    model = Sam3Model.from_pretrained(SAM3_ID).to(dev).eval()

    def detect(rgb):
        image = Image.fromarray(rgb)
        # One batch entry per phrase: the same picture, a different concept.
        inputs = processor(images=[image] * len(phrases), text=phrases,
                           return_tensors="pt").to(dev)
        with torch.inference_mode():
            outputs = model(**inputs)
        results = processor.post_process_instance_segmentation(
            outputs, threshold=args.threshold, mask_threshold=args.mask_threshold,
            target_sizes=[(H, W)] * len(phrases))
        return {p: union_masks(r["masks"], H, W) for p, r in zip(phrases, results)}

    return detect


def sam2_detector(dev, args, caption, phrases, H, W):
    """Grounding DINO (text -> boxes) + SAM 2.1 (boxes -> masks)."""
    from transformers import AutoProcessor, AutoModelForZeroShotObjectDetection
    from sam2.sam2_image_predictor import SAM2ImagePredictor

    print(f"[maskgen] loading Grounding DINO {args.dino} (first run downloads the weights)...", flush=True)
    dino_proc = AutoProcessor.from_pretrained(DINO_IDS[args.dino])
    dino = AutoModelForZeroShotObjectDetection.from_pretrained(DINO_IDS[args.dino]).to(dev).eval()
    print(f"[maskgen] loading SAM 2.1 hiera-{args.sam}...", flush=True)
    # SAM2Transforms jit-scripts its resize/normalize chain. Under an embedded
    # interpreter inspect.getsource() cannot read torchvision's enum source, so
    # the scripting raises OSError. The chain is plain preprocessing and behaves
    # identically eager, so scripting is disabled just for construction.
    scripted = torch.jit.script
    torch.jit.script = lambda obj, *a, **k: obj
    try:
        sam = SAM2ImagePredictor.from_pretrained(SAM_IDS[args.sam], device=dev)
    finally:
        torch.jit.script = scripted

    def match_phrase(label):
        """Grounding DINO labels are the matched text; map back to a prompt phrase."""
        label = (label or "").lower().strip()
        for p in phrases:
            if p == label or p in label or label in p:
                return p
        return phrases[0]

    def detect(rgb):
        per_phrase = {p: np.zeros((H, W), np.float32) for p in phrases}
        inputs = dino_proc(images=Image.fromarray(rgb), text=caption, return_tensors="pt").to(dev)
        with torch.inference_mode():
            outputs = dino(**inputs)
        results = dino_proc.post_process_grounded_object_detection(
            outputs, inputs["input_ids"],
            threshold=args.threshold, text_threshold=args.text_threshold,
            target_sizes=[(H, W)])
        boxes = results[0]["boxes"].detach().cpu().numpy().astype(np.float32)
        if len(boxes) == 0:
            return per_phrase
        labels = results[0].get("text_labels") or results[0].get("labels") or []
        labels = [match_phrase(l if isinstance(l, str) else str(l)) for l in labels]
        sam.set_image(rgb)
        for p in phrases:
            sel = np.array([lab == p for lab in labels], dtype=bool)
            if not sel.any():
                continue
            with torch.inference_mode():
                masks, _, _ = sam.predict(box=boxes[sel], multimask_output=False)
            per_phrase[p] = union_masks(masks, H, W)
        return per_phrase

    return detect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--prompt", default="person",
                    help='what to segment, e.g. "person. face." (periods or commas separate phrases)')
    ap.add_argument("--out", default=None)
    ap.add_argument("--model", choices=("auto", "sam3", "sam2"), default="auto",
                    help="auto = SAM 3 when reachable, Grounding DINO + SAM 2.1 otherwise")
    ap.add_argument("--threshold", type=float, default=0.30,
                    help="match confidence (lower = more, looser detections); "
                         "SAM 3's instance score, or Grounding DINO's box score")
    ap.add_argument("--mask-threshold", type=float, default=0.50,
                    help="SAM 3 only: cutoff that binarizes a predicted mask")
    ap.add_argument("--text-threshold", type=float, default=0.25,
                    help="SAM 2.1 only: Grounding DINO phrase-match confidence")
    ap.add_argument("--dino", choices=sorted(DINO_IDS), default="base",
                    help="SAM 2.1 only: Grounding DINO size (base detects far more reliably)")
    ap.add_argument("--sam", choices=sorted(SAM_IDS), default="large",
                    help="SAM 2.1 only: SAM 2.1 size (large gives the cleanest edges)")
    ap.add_argument("--detect-every", type=int, default=1,
                    help="run the model every N frames (1 = every frame)")
    ap.add_argument("--feather", type=int, default=0,
                    help="soften the mask edge by N pixels (0 = hard edge)")
    ap.add_argument("--invert", action="store_true",
                    help="write the complement: process everything EXCEPT the prompt")
    ap.add_argument("--layers", action="store_true",
                    help="also write one <stem>_mask_<phrase>.mp4 per phrase")
    args = ap.parse_args()

    if args.detect_every < 1:
        sys.exit("--detect-every must be >= 1")

    src = args.input
    stem = os.path.splitext(src)[0]
    out = args.out or stem + "_mask.mp4"
    caption, phrases = normalize_prompt(args.prompt)
    ffmpeg, ffprobe = ffmpeg_tools()

    probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height,r_frame_rate",
                            "-of", "csv=p=0", src], capture_output=True, text=True, check=True)
    w, h, fr = probe.stdout.strip().split(",")[:3]
    W, H = int(w), int(h)
    num, den = fr.split("/")
    fps = float(num) / float(den or 1)
    print(f"[maskgen] {W}x{H} @ {fps:g} fps -> {out}", flush=True)
    print(f"[maskgen] prompt: {caption!r} ({len(phrases)} phrase(s))", flush=True)

    dev = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    detect = None
    if args.model in ("auto", "sam3"):
        print("[maskgen] loading SAM 3 (first run downloads the weights)...", flush=True)
        try:
            detect = sam3_detector(dev, args, phrases, H, W)
            backend = "SAM 3"
        except Exception as e:
            text = f"{type(e).__name__}: {e}"
            gated = looks_gated(text)
            if args.model == "sam3":
                sys.exit(f"[maskgen] cannot load {SAM3_ID}. "
                         f"{GATED_HELP if gated else ''}\n[maskgen] underlying error: {text}")
            print(f"[maskgen] SAM 3 is {'gated for this account' if gated else 'unavailable'}; "
                  f"falling back to Grounding DINO + SAM 2.1.", flush=True)
            if gated:
                print(f"[maskgen] {GATED_HELP}", flush=True)
            else:
                print(f"[maskgen] underlying error: {text}", flush=True)
    if detect is None:
        detect = sam2_detector(dev, args, caption, phrases, H, W)
        backend = "Grounding DINO + SAM 2.1"
    print(f"[maskgen] backend={backend} threshold={args.threshold:g}", flush=True)

    def encoder(path):
        return subprocess.Popen([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                 "-f", "rawvideo", "-pix_fmt", "gray", "-video_size", f"{W}x{H}",
                                 "-framerate", f"{fps}", "-i", "-",
                                 "-c:v", "libx264", "-preset", "fast", "-crf", "6",
                                 "-pix_fmt", "yuv420p", path],
                                stdin=subprocess.PIPE)

    dec = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", src,
                            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                           stdout=subprocess.PIPE)
    enc = encoder(out)
    # One extra writer per phrase for --layers. Names are per phrase, so two
    # phrases that slug alike ("car", "car!") share a layer rather than clash.
    layer_paths = {}
    layer_enc = {}
    if args.layers:
        for p in phrases:
            path = f"{stem}_mask_{phrase_slug(p)}.mp4"
            if path not in layer_enc:
                layer_enc[path] = encoder(path)
            layer_paths[p] = path
        for path in layer_enc:
            print(f"[maskgen] layer -> {path}", flush=True)

    def union(per_phrase):
        """Every phrase's mask merged into the one the NR pass receives."""
        merged = np.zeros((H, W), np.float32)
        for m in per_phrase.values():
            np.maximum(merged, m, out=merged)
        return merged

    def feather(mask, radius):
        """Cheap separable box blur; keeps soft edges the shader can use directly."""
        if radius <= 0:
            return mask
        k = 2 * radius + 1
        pad = np.pad(mask, radius, mode="edge")
        cs = np.cumsum(pad, axis=0)
        blur = (cs[k - 1:, :] - np.vstack([np.zeros((1, pad.shape[1]), np.float32), cs[:-k, :]])) / k
        cs = np.cumsum(blur, axis=1)
        blur = (cs[:, k - 1:] - np.hstack([np.zeros((blur.shape[0], 1), np.float32), cs[:, :-k]])) / k
        return np.clip(blur, 0.0, 1.0)

    def finish(mask, prev, reset):
        """Feather and temporally blend one mask plane; returns the new plane."""
        mask = feather(mask, args.feather)
        if prev is not None and not reset:
            mask = prev * MASK_HISTORY + mask * (1.0 - MASK_HISTORY)
        return mask

    def write(e, mask):
        written = 1.0 - mask if args.invert else mask
        e.stdin.write((written * 255.0 + 0.5).astype(np.uint8).tobytes())

    frame_bytes = W * H * 3
    prev_gray = None
    prev_mask = None
    prev_layer = {p: None for p in layer_paths}
    detected = {p: np.zeros((H, W), np.float32) for p in phrases}
    n = 0
    empty_frames = 0
    while True:
        buf = dec.stdout.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        # .copy() because np.frombuffer hands out a READ-ONLY view, and torch
        # warns that writing through a tensor made from one is undefined.
        rgb = np.frombuffer(buf, np.uint8).reshape(H, W, 3).copy()
        gray = rgb.mean(axis=2).astype(np.float32)
        reset = n == 0
        if prev_gray is not None:
            reset = float(np.abs(gray - prev_gray).mean()) / 255.0 >= SCENE_CUT_THRESHOLD

        # A cut invalidates carried-over masks, so always re-detect across one.
        if reset or n % args.detect_every == 0:
            detected = detect(rgb)
        merged = union(detected)
        if not merged.any():
            empty_frames += 1
        mask = finish(merged, prev_mask, reset)
        prev_mask = mask
        write(enc, mask)

        for p, path in layer_paths.items():
            layer = finish(detected[p], prev_layer[p], reset)
            prev_layer[p] = layer
            write(layer_enc[path], layer)

        prev_gray = gray
        n += 1
        if n % 30 == 0:
            print(f"[maskgen] frame {n}", flush=True)

    dec.stdout.close()
    enc.stdin.close()
    for e in layer_enc.values():
        e.stdin.close()
    enc.wait()
    for e in layer_enc.values():
        e.wait()
    dec.wait()
    if n and empty_frames == n:
        print(f"[maskgen] WARNING: nothing matched {caption!r} in any frame; "
              f"the mask is empty. Try a lower --threshold or simpler phrases.", flush=True)
    elif empty_frames:
        print(f"[maskgen] note: {empty_frames}/{n} frames had no detection", flush=True)
    print(f"[maskgen] done frames={n} backend={backend} -> {out}", flush=True)
    return 0 if n > 0 and enc.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
