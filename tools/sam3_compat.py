"""Run Meta's SAM 3.1 (facebookresearch/sam3) inside this project's Python.

SAM 3.1 ships as checkpoints only: unlike SAM 3, there is no `transformers`
integration, so the model has to be built by Meta's own package. That package is
a research checkout rather than a wheel, and three things stand between it and a
stock ComfyUI interpreter. All three are handled here, in process, so nothing is
installed into or downgraded in the interpreter ComfyUI shares:

  * setuptools 83 removed `pkg_resources`, which `sam3/model_builder.py` imports
    only to locate the bundled BPE vocabulary.
  * `model_builder` transitively imports the TRAINING data pipeline, which pulls
    `pycocotools` and friends. None of it runs at inference, so it is stubbed.
  * `sam3/model/decoder.py` pins its attention to `SDPBackend.FLASH_ATTENTION`
    alone, and flash has no kernel for some of the shapes the multiplex tracker
    produces, which aborts propagation with "No available kernel". The same
    repository already uses a permissive backend list in `vl_combiner.py`, so
    `relax_sdpa` widens this one to match.

Beyond that, two facts about the released checkpoint decide how it is used:

  * `sam3.1_multiplex.pt` carries `detector.*` and `tracker.*` weights for the
    MULTIPLEX VIDEO model. Building the plain image model from it silently
    leaves one neck level randomly initialised (the image model wants four
    pyramid convolutions, the checkpoint has three), so the video predictor is
    the only correct entry point.
  * `sam3_base_predictor.start_session` passes `offload_state_to_cpu`, which the
    multiplex tracker's `init_state` does not accept, so `start_session` here
    passes only the arguments that signature actually takes.

Masks come back from the predictor as numpy BOOL arrays under
`out_binary_masks`, shaped (instances, ..., H, W).
"""
from __future__ import annotations

import contextlib
import importlib.machinery
import importlib.util
import io
import os
import sys
import time
import types
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CKPT_NAME = "sam3.1_multiplex.pt"

# Training-only modules the inference import chain touches, as
# name -> (submodules, callables). The callables raise if anything reaches them.
_STUBS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "pycocotools": (("mask",), ()),
    "numba": ((), ("njit", "jit", "prange")),
    "rapidjson": ((), ("loads", "dumps")),
    "python_rapidjson": ((), ("loads", "dumps")),
}


def sam3_dir() -> Path | None:
    """The facebookresearch/sam3 checkout, or None when it is not present.

    Order: SMRU_SAM3_DIR, then external\\sam3 in this checkout.
    """
    env = os.environ.get("SMRU_SAM3_DIR")
    for cand in ([Path(env)] if env else []) + [REPO_ROOT / "external" / "sam3"]:
        if (cand / "sam3" / "model_builder.py").is_file():
            return cand
    return None


def checkpoint_path() -> Path | None:
    """The SAM 3.1 checkpoint, or None when it cannot be found.

    Order: SMRU_SAM31_CKPT, the models folder the player publishes through
    HF_HOME's parent, `models\\` beside this checkout, then the checkout root.
    """
    env = os.environ.get("SMRU_SAM31_CKPT")
    cands = [Path(env)] if env else []
    hf = os.environ.get("HF_HOME")
    if hf:
        cands.append(Path(hf).parent / CKPT_NAME)
    cands += [REPO_ROOT / "models" / CKPT_NAME, REPO_ROOT / CKPT_NAME]
    for c in cands:
        if c.is_file():
            return c
    return None


def available() -> bool:
    """True when both the package checkout and a checkpoint are in place."""
    return sam3_dir() is not None and checkpoint_path() is not None


def _installed(top: str) -> bool:
    """True if the real package is importable and is not one of our stubs."""
    mod = sys.modules.get(top)
    if mod is not None:
        return not getattr(mod, "__is_smru_stub__", False)
    try:
        return importlib.util.find_spec(top) is not None
    except (ImportError, ValueError):
        return False


def _new_module(name: str) -> types.ModuleType:
    mod = types.ModuleType(name)
    mod.__is_smru_stub__ = True
    mod.__spec__ = importlib.machinery.ModuleSpec(name, None)
    mod.__file__ = None
    mod.__path__ = []

    def __getattr__(attr: str, _n: str = name) -> object:
        # torch walks sys.modules with inspect() while registering custom ops;
        # dunder probes must look absent or it trips over a non-string __file__.
        if attr.startswith("__") and attr.endswith("__"):
            raise AttributeError(attr)
        raise RuntimeError(f"{_n}.{attr} is a stub: training-only code is unavailable at inference")

    mod.__getattr__ = __getattr__
    return mod


def _stub(name: str, submodules: tuple[str, ...], callables: tuple[str, ...]) -> None:
    if name in sys.modules or _installed(name.split(".")[0]):
        return
    mod = _new_module(name)
    for sub in submodules:
        child = _new_module(f"{name}.{sub}")
        setattr(mod, sub, child)
        sys.modules[f"{name}.{sub}"] = child
    for fn in callables:
        def _refuse(*a, _n: str = f"{name}.{fn}", **k):
            raise RuntimeError(f"{_n} is a stub: training-only code is unavailable at inference")
        setattr(mod, fn, _refuse)
    sys.modules[name] = mod


def prepare() -> Path:
    """Install the shims and put the sam3 checkout on sys.path. Returns its path."""
    d = sam3_dir()
    if d is None:
        raise FileNotFoundError(
            "the facebookresearch/sam3 checkout was not found. Clone it into "
            f"{REPO_ROOT / 'external' / 'sam3'} (git clone --depth 1 "
            "https://github.com/facebookresearch/sam3.git), or set SMRU_SAM3_DIR."
        )

    if not _installed("pkg_resources"):
        shim = _new_module("pkg_resources")

        def resource_filename(package: str, resource: str) -> str:
            spec = importlib.util.find_spec(package)
            if spec is None or not spec.origin:
                raise ModuleNotFoundError(package)
            return str(Path(spec.origin).parent / resource)

        shim.resource_filename = resource_filename
        sys.modules["pkg_resources"] = shim

    for name, (subs, fns) in _STUBS.items():
        _stub(name, subs, fns)

    if str(d) not in sys.path:
        sys.path.insert(0, str(d))
    return d


def relax_sdpa() -> bool:
    """Let the tracker's attention fall back off FlashAttention. Import sam3 first."""
    try:
        import sam3.model.decoder as decoder
        from torch.nn.attention import SDPBackend, sdpa_kernel
    except ImportError:
        return False

    order = [SDPBackend.FLASH_ATTENTION, SDPBackend.EFFICIENT_ATTENTION, SDPBackend.MATH]

    def permissive(*_args, **_kwargs):
        return sdpa_kernel(order)

    decoder.sdpa_kernel = permissive
    return True


def build_predictor(ckpt: Path | str | None = None, quiet: bool = True,
                    max_num_objects: int = 16):
    """Build the SAM 3.1 multiplex video predictor from a local checkpoint.

    The whole model runs under bfloat16 autocast, as Meta's own examples do;
    the caller is expected to have entered that context (see `autocast`).

    `max_num_objects` is the tracker's instance budget per pass. It is a real
    ceiling, not a hint: once it is full the tracker logs "hitting
    max_num_objects" and drops NEW detections, so a crowd scene prompted with
    "person" keeps only the first faces it locked onto. Raising it costs memory
    and time roughly linearly.
    """
    d = prepare()
    ckpt = Path(ckpt) if ckpt else checkpoint_path()
    if ckpt is None or not Path(ckpt).is_file():
        raise FileNotFoundError(
            f"{CKPT_NAME} was not found. Put it in {REPO_ROOT / 'models'} or set SMRU_SAM31_CKPT. "
            "It is gated: request access at https://huggingface.co/facebook/sam3.1"
        )
    from sam3.model_builder import build_sam3_multiplex_video_predictor

    relax_sdpa()
    bpe = str(d / "sam3" / "assets" / "bpe_simple_vocab_16e6.txt.gz")
    # The builder prints its whole missing-key list (the tracker shares the
    # detector's backbone, which is deleted straight after, so those are
    # expected); ~170 kB of noise that would drown the job's own progress.
    sink = io.StringIO() if quiet else sys.stdout
    with contextlib.redirect_stdout(sink):
        predictor = build_sam3_multiplex_video_predictor(
            checkpoint_path=str(ckpt), bpe_path=bpe,
            max_num_objects=max_num_objects,
            # multiplex_count is the per-bucket capacity and is baked into the
            # weights: changing it makes the checkpoint fail to load. Only the
            # object budget above is tunable; the tracker adds more buckets.
            use_fa3=False,        # FlashAttention 3 is not installed on Windows
            compile=False, warm_up=False,
        )
    return predictor


def autocast():
    """The bfloat16 autocast context the model expects, as in Meta's examples."""
    import torch

    if torch.cuda.is_available():
        torch.backends.cuda.matmul.allow_tf32 = True
        torch.backends.cudnn.allow_tf32 = True
        return torch.autocast("cuda", dtype=torch.bfloat16)
    return contextlib.nullcontext()


def start_session(predictor, video_path: str, offload_video_to_cpu: bool = True) -> tuple[str, int]:
    """Open a session on a video. Returns (session_id, frame_count).

    `sam3_base_predictor.start_session` hands `init_state` an
    `offload_state_to_cpu` argument the multiplex tracker does not accept, so
    the session is opened here with only the arguments its signature takes and
    registered exactly the way the base predictor registers one.
    """
    import inspect

    sig = inspect.signature(predictor.model.init_state).parameters
    kwargs = {"resource_path": video_path}
    wanted = {
        "offload_video_to_cpu": offload_video_to_cpu,
        "input_is_mp4": str(video_path).lower().endswith(".mp4"),
        "async_loading_frames": getattr(predictor, "async_loading_frames", None),
    }
    for k, v in wanted.items():
        if k in sig and v is not None:
            kwargs[k] = v
    state = predictor.model.init_state(**kwargs)
    sid = str(uuid.uuid4())
    now = time.time()
    predictor._all_inference_states[sid] = {
        "state": state, "session_id": sid, "start_time": now, "last_use_time": now,
    }
    frames = int(state["num_frames"]) if isinstance(state, dict) and "num_frames" in state else 0
    return sid, frames


def close_session(predictor, session_id: str) -> None:
    with contextlib.suppress(Exception):
        predictor.handle_request(request=dict(type="close_session", session_id=session_id))
