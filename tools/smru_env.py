"""Shared environment helpers for the SmackMyRezUp tool scripts.

Nothing here is a fixed machine path. The binaries are located, in order, via
the SMRU_BIN_DIR environment variable (the player exports it for every child
process it launches), the checkout's build\\Release folder, the checkout root,
and finally PATH.
"""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
EXPORTER_EXE = "SmackMyRezUpExport.exe"


def bin_dir() -> Path | None:
    """Folder holding ffmpeg.exe / ffprobe.exe / the exporter, or None."""
    candidates: list[Path] = []
    env = os.environ.get("SMRU_BIN_DIR")
    if env:
        candidates.append(Path(env))
    candidates += [REPO_ROOT / "build" / "Release", REPO_ROOT]
    for c in candidates:
        if (c / "ffmpeg.exe").is_file():
            return c
    return None


def ffmpeg_tools() -> tuple[str, str]:
    """Absolute (ffmpeg, ffprobe) paths; exits with a clear message when missing."""
    d = bin_dir()
    if d is not None:
        return str(d / "ffmpeg.exe"), str(d / "ffprobe.exe")
    ff, fp = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if ff and fp:
        return ff, fp
    sys.exit("ffmpeg/ffprobe not found: build the player first or set SMRU_BIN_DIR")


def exporter_exe() -> str:
    """Path to SmackMyRezUpExport.exe; exits with a clear message when missing."""
    d = bin_dir()
    if d is not None and (d / EXPORTER_EXE).is_file():
        return str(d / EXPORTER_EXE)
    sys.exit(f"{EXPORTER_EXE} not found: build the player first or set SMRU_BIN_DIR")


def comfyui_root() -> Path | None:
    """ComfyUI portable root (the folder that holds python_embeded\\ and ComfyUI\\).

    SMRU_COMFYUI_DIR wins; otherwise, when this script runs under ComfyUI's
    embedded interpreter, the root is derived from sys.executable.
    """
    env = os.environ.get("SMRU_COMFYUI_DIR")
    if env and Path(env).is_dir():
        return Path(env)
    exe = Path(sys.executable).resolve()
    if exe.parent.name.lower() == "python_embeded" and (exe.parents[1] / "ComfyUI").is_dir():
        return exe.parents[1]
    return None


# Interpreters that are known to carry torch, in the order AppPaths.h resolves
# them for the player's child processes, so the tools agree with the GUI.
_KNOWN_COMFY_ROOTS = (
    r"X:\comfyui\comfyui\ComfyUI_windows_portable",
    r"C:\ComfyUI_windows_portable",
    r"D:\ComfyUI_windows_portable",
)
_REEXEC_FLAG = "SMRU_TORCH_REEXEC"


def _has_torch(exe: Path) -> bool:
    import subprocess
    try:
        return subprocess.run([str(exe), "-c", "import torch"],
                              capture_output=True, timeout=180).returncode == 0
    except Exception:
        return False


def torch_python() -> str | None:
    """An interpreter with torch installed, or None if none can be found."""
    cands: list[Path] = []
    env = os.environ.get("SMRU_PYTHON")
    if env:
        cands.append(Path(env))
    root = os.environ.get("SMRU_COMFYUI_DIR")
    if root:
        cands.append(Path(root) / "python_embeded" / "python.exe")
    r = comfyui_root()
    if r is not None:
        cands.append(r / "python_embeded" / "python.exe")
    cands += [Path(p) / "python_embeded" / "python.exe" for p in _KNOWN_COMFY_ROOTS]
    seen = set()
    for c in cands:
        c = Path(c)
        if c in seen or not c.is_file():
            continue
        seen.add(c)
        if _has_torch(c):
            return str(c)
    return None


def ensure_torch() -> None:
    """Re-run this script under an interpreter that has torch, if this one lacks it.

    These tools are usually launched by the player, which resolves the right
    interpreter itself. Run by hand, `python tools\\...` picks up whatever is on
    PATH - typically a system Python with no torch - and the script dies on its
    import with a message that says nothing about which Python to use instead.
    Rather than fail, hand the job to the interpreter the player would have
    chosen. Call this BEFORE importing torch.
    """
    import subprocess
    try:
        import torch  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    if os.environ.get(_REEXEC_FLAG):        # already re-run once; do not loop
        sys.exit("[smru] the interpreter chosen for torch does not have it after all")
    exe = torch_python()
    if exe is None:
        sys.exit("[smru] this Python has no torch, and no interpreter with torch was found.\n"
                 "       Point SMRU_PYTHON at one (ComfyUI's python_embeded\\python.exe is the\n"
                 "       reference setup), or set SMRU_COMFYUI_DIR to a ComfyUI portable root.")
    print(f"[smru] this Python has no torch; re-running under {exe}", flush=True)
    env = dict(os.environ, **{_REEXEC_FLAG: "1"})
    sys.exit(subprocess.run([exe, os.path.abspath(sys.argv[0])] + sys.argv[1:], env=env).returncode)
