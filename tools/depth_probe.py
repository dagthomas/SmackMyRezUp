# Depth-consumption probe for the SmackMyRezUp neural pipeline.
#
# Run after updating nvngx_dlssnr.dll:
#   python tools\depth_probe.py
#
# Feeds the same frames twice with OPPOSITE extreme depth planes (all-near vs
# all-far) through SmackMyRezUpExport --depth-in. As of 2026-08-31 the result is
# 0.0: the NR/DLSS path ignores depth content entirely. The moment this prints a
# non-zero diff, the runtime has started consuming depth - wire the Depth
# Anything sidecar in by default and real geometry will matter.
import numpy as np, os, subprocess, sys
# ComfyUI's embedded Python does not put the script folder on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smru_env import exporter_exe

EXE = exporter_exe()
REL = os.path.dirname(EXE)
W, H, N = 640, 360, 8

rng = np.random.default_rng(7)
base = rng.integers(0, 256, (H, W, 4), dtype=np.uint8)
base[..., 3] = 255
frames = [np.roll(base, i * 3, axis=1) for i in range(N)]

def payload(depth_val):
    dep = np.full((H, W), depth_val, np.uint16)
    parts = []
    for f in frames:
        parts.append(f.tobytes())
        parts.append(dep.tobytes())
    return b"".join(parts)

def run(pl):
    p = subprocess.run([EXE, "--raw", "--size", f"{W}x{H}", "--fps", "24",
                        "--tone", "nr", "--mv", "estimated", "--depth-in"],
                       input=pl, capture_output=True, cwd=REL, timeout=400)
    if p.returncode != 0:
        sys.exit("exporter failed: " + p.stderr.decode(errors="replace")[-300:])
    return np.frombuffer(p.stdout, np.uint8).astype(np.int16)

diff = np.abs(run(payload(0)) - run(payload(65535))).mean()
print(f"all-near vs all-far mean|diff| = {diff:.5f}")
print("depth is CONSUMED - wire Depth Anything!" if diff > 0.01 else
      "depth still ignored by this addon/DLL build")
