"""Locate the TensorRT runtime, build engines from ONNX and run them.

Engines are not portable. TensorRT compiles for one GPU architecture and one
runtime version, so nothing here can ship prebuilt: every engine is built on
first use and cached under engines/ with the GPU, the TensorRT version, the
precision and the input shape in its file name. A driver, hardware or model
change therefore produces a new file instead of a silent mismatch.

Nothing depends on where the checkout or the SDK lives. The runtime DLLs are
found through SMRU_TENSORRT_DIR (root or bin folder), then by looking for
nvinfer_*.dll along PATH; the engine cache follows SMRU_ENGINE_DIR, else it
sits beside the checkout.

Device buffers are plain torch CUDA tensors - torch is already a hard
dependency of every generator here, and using its allocator keeps the engines
on the same stream as any torch pre/post-processing without a second CUDA
binding in the process.
"""
from __future__ import annotations

import glob
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# Bump when anything in build_engine's configuration changes: it is part of
# every engine file name, so old engines are ignored instead of being reused
# with settings they were not built under.
BUILD_REVISION = 1

_trt = None          # the imported tensorrt module, once it has been located
_logger = None       # TensorRT keeps one global logger; making a second only warns
_TRT_TO_TORCH = {}   # filled in on first import, keyed by trt.DataType


def tensorrt_dir() -> Path | None:
    """Folder holding nvinfer_*.dll, or None when TensorRT is not installed."""
    env = os.environ.get("SMRU_TENSORRT_DIR")
    if env:
        root = Path(env)
        for cand in (root, root / "bin", root / "lib"):
            if glob.glob(str(cand / "nvinfer_*.dll")):
                return cand
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if entry and glob.glob(os.path.join(entry, "nvinfer_*.dll")):
            return Path(entry)
    return None


def import_tensorrt():
    """Import tensorrt with its DLL folder registered; exits with advice."""
    global _trt, _TRT_TO_TORCH
    if _trt is not None:
        return _trt
    d = tensorrt_dir()
    if d is not None and hasattr(os, "add_dll_directory"):
        try:
            os.add_dll_directory(str(d))
        except OSError:
            pass
    try:
        import tensorrt as trt
    except ImportError:
        sys.exit("TensorRT is not installed in this interpreter. Install the wheel "
                 "from the SDK (python\\tensorrt-*-cp3XX-none-win_amd64.whl) and make "
                 "sure its bin folder is on PATH or in SMRU_TENSORRT_DIR.")
    import torch
    _TRT_TO_TORCH = {
        trt.DataType.FLOAT: torch.float32,
        trt.DataType.HALF: torch.float16,
        trt.DataType.INT8: torch.int8,
        trt.DataType.INT32: torch.int32,
        trt.DataType.INT64: torch.int64,
        trt.DataType.BOOL: torch.bool,
        trt.DataType.UINT8: torch.uint8,
    }
    _trt = trt
    return trt


def logger():
    """The one TensorRT logger this process uses."""
    global _logger
    if _logger is None:
        _logger = import_tensorrt().Logger(import_tensorrt().Logger.WARNING)
    return _logger


def engine_dir() -> Path:
    """Where built engines are cached (created on demand)."""
    d = Path(os.environ.get("SMRU_ENGINE_DIR") or (REPO_ROOT / "engines"))
    d.mkdir(parents=True, exist_ok=True)
    return d


def device_tag() -> str:
    """Short, file-name-safe identity of the GPU an engine was built for."""
    import torch
    if not torch.cuda.is_available():
        return "nocuda"
    name = torch.cuda.get_device_name(0)
    major, minor = torch.cuda.get_device_capability(0)
    safe = "".join(c for c in name.replace("NVIDIA GeForce", "") if c.isalnum())
    return f"{safe}-sm{major}{minor}"


def engine_path(tag: str, shape, fp16: bool) -> Path:
    """Cache path for one model at one input shape on this machine."""
    trt = import_tensorrt()
    dims = "x".join(str(int(v)) for v in shape)
    prec = "fp16" if fp16 else "fp32"
    return (engine_dir() /
            f"{tag}-{dims}-{prec}-{device_tag()}-trt{trt.__version__}-b{BUILD_REVISION}.engine")


def build_engine(onnx_path, out_path, tf32=True, edge_mask_convs=False, opt_level=None,
                 workspace_gb=8, log=print) -> Path:
    """Compile an ONNX file into a serialized engine. Returns out_path.

    There is no precision flag. TensorRT 11 networks are strongly typed and it
    removed BuilderFlag.FP16 along with the rest of the precision flags, so the
    arithmetic is whatever the ONNX graph declares. Half precision therefore
    has to come from the export - see the fp16 argument threaded through
    load_or_build into each model's exporter.
    """
    trt = import_tensorrt()
    onnx_path, out_path = Path(onnx_path), Path(out_path)
    builder = trt.Builder(logger())
    network = builder.create_network(0)
    parser = trt.OnnxParser(network, logger())
    # parse_from_file, not parse(bytes): the exporter writes weights over 2 GB
    # (and often smaller ones) to a sibling .onnx.data file, and only the file
    # form gives the parser the directory it needs to resolve them.
    if not parser.parse_from_file(str(onnx_path)):
        errs = "\n".join(str(parser.get_error(i)) for i in range(parser.num_errors))
        sys.exit(f"TensorRT could not parse {onnx_path.name}:\n{errs}")
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, int(workspace_gb * (1 << 30)))
    if not tf32:
        # TF32 keeps 10 mantissa bits and is on by default. Measured on RAFT it
        # is not what costs accuracy here (0.0638 vs 0.0628 px mean deviation
        # from torch, with and without), so it stays on for the speed.
        config.clear_flag(trt.BuilderFlag.TF32)
    if not edge_mask_convs:
        # Kept switchable for experiments only: excluding this tactic family
        # (or JIT convolutions, or both) made no difference to the RAFT
        # problem described in load_or_build. The default stays off simply
        # because the sweep found no engine that was faster with it on.
        config.set_tactic_sources(config.get_tactic_sources()
                                  & ~(1 << int(trt.TacticSource.EDGE_MASK_CONVOLUTIONS)))
    if opt_level is not None:
        # Level 0 skips the timing-based kernel search, which is what makes
        # its output reproducible from one build to the next (measured on
        # RAFT: bit-identical engines, 6 s builds, ~20% slower than the best
        # autotuned engine and within 0.03 px of it).
        config.builder_optimization_level = opt_level
    level = "default" if opt_level is None else str(opt_level)
    log(f"[trt] building {out_path.name} (optimization level {level})...")
    plan = builder.build_serialized_network(network, config)
    if plan is None:
        sys.exit(f"TensorRT failed to build an engine from {onnx_path.name}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + ".part")
    with open(tmp, "wb") as f:
        f.write(plan)
    os.replace(tmp, out_path)          # never leave a half-written engine behind
    log(f"[trt] built {out_path.name} ({out_path.stat().st_size / (1 << 20):.0f} MB)")
    return out_path


class Engine:
    """A loaded engine; inputs and outputs are torch CUDA tensors."""

    def __init__(self, path, device=None):
        trt = import_tensorrt()
        import torch
        self.torch = torch
        self.device = device or torch.device("cuda")
        self.runtime = trt.Runtime(logger())
        with open(path, "rb") as f:
            self.engine = self.runtime.deserialize_cuda_engine(f.read())
        if self.engine is None:
            sys.exit(f"TensorRT could not deserialize {Path(path).name}. Delete it and "
                     f"rebuild - engines are tied to one GPU and one TensorRT version.")
        self.context = self.engine.create_execution_context()
        self.inputs, self.outputs = [], []
        for i in range(self.engine.num_io_tensors):
            name = self.engine.get_tensor_name(i)
            if self.engine.get_tensor_mode(name) == trt.TensorIOMode.INPUT:
                self.inputs.append(name)
            else:
                self.outputs.append(name)
        self._out_buffers = {}
        # TensorRT warns about enqueueing on the default stream, where it has to
        # insert extra synchronization. A stream of its own avoids that; the
        # wait_stream pair below keeps it ordered against torch's own work.
        self.stream = torch.cuda.Stream(device=self.device)

    def dtype_of(self, name):
        return _TRT_TO_TORCH[self.engine.get_tensor_dtype(name)]

    def __call__(self, **feeds):
        """Run one inference. Feeds and results are torch CUDA tensors."""
        torch = self.torch
        held = []
        for name in self.inputs:
            t = feeds[name].to(self.device, self.dtype_of(name)).contiguous()
            held.append(t)                       # keep casts alive until execute
            self.context.set_input_shape(name, tuple(t.shape))
            self.context.set_tensor_address(name, t.data_ptr())
        results = {}
        for name in self.outputs:
            shape = tuple(self.context.get_tensor_shape(name))
            buf = self._out_buffers.get(name)
            if buf is None or tuple(buf.shape) != shape:
                buf = torch.empty(shape, dtype=self.dtype_of(name), device=self.device)
                self._out_buffers[name] = buf
            self.context.set_tensor_address(name, buf.data_ptr())
            results[name] = buf
        self.stream.wait_stream(torch.cuda.current_stream(self.device))
        if not self.context.execute_async_v3(self.stream.cuda_stream):
            raise RuntimeError("TensorRT execute_async_v3 failed")
        self.stream.synchronize()
        del held
        return results


def load_or_build(tag, shape, onnx_builder, fp16=True, verify=None, tolerance=0.0,
                  opt_levels=(None, 0), log=print) -> Engine:
    """Return the cached engine for (tag, shape, precision), building it once.

    onnx_builder(path, fp16) is called only on a cache miss and must write an
    ONNX file at that path in the precision it is handed. The ONNX is kept next
    to the engine, so rebuilding after a TensorRT or driver upgrade costs a
    compile but not another export.

    verify(engine) -> float is optional but strongly recommended for any model
    whose output feeds the neural pass. TensorRT chooses kernels by timing
    them, and for some graphs that choice is not reproducible: the same RAFT
    ONNX built twelve times gave three engines that agree with torch to 0.06 px
    and nine that wander 0.4 px on average and 80 px at the frame border, with
    nothing in the build configuration to tell them apart (tactic sources, TF32
    and edge-mask convolutions were all ruled out). So the engine is measured
    against the model it came from and one that deviates by more than
    `tolerance` is discarded. The check runs on cache hits too, because a bad
    engine that reached the disk would otherwise be reused forever.

    opt_levels lists the builder optimization levels tried in order, one build
    each. The default pair is one fully autotuned attempt (fastest engine when
    it lands) followed by level 0, which skips the timing search and is
    reproducible - on RAFT it lands within 0.03 px of the best autotuned engine
    every time.
    """
    eng = engine_path(tag, shape, fp16)
    dims = "x".join(str(int(v)) for v in shape)
    onnx = engine_dir() / f"{tag}-{dims}-{'fp16' if fp16 else 'fp32'}.onnx"
    if not onnx.is_file() and not eng.is_file():
        log(f"[trt] exporting {onnx.name}...")
        onnx_builder(onnx, fp16)
    levels = list(opt_levels)
    if eng.is_file():
        levels.insert(0, "cached")
    for level in levels:
        if level != "cached":
            if not onnx.is_file():
                log(f"[trt] exporting {onnx.name}...")
                onnx_builder(onnx, fp16)
            build_engine(onnx, eng, opt_level=level, log=log)
        engine = Engine(eng)
        if verify is None:
            return engine
        err = verify(engine)
        if err <= tolerance:
            log(f"[trt] engine verified: {err:.4g} <= {tolerance:.4g}")
            return engine
        del engine
        log(f"[trt] engine deviates from the reference by {err:.4g} (limit {tolerance:.4g}); "
            f"discarding")
        eng.unlink()
    sys.exit(f"TensorRT could not build an engine for {tag} that matches the model within "
             f"{tolerance:.4g}. Use the torch backend.")
