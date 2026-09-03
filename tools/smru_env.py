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
