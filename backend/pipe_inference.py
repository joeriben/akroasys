#!/usr/bin/env python3
"""Pipe-based inference for T5ynth — stdin JSON requests, stdout binary audio.

Protocol:
  Request:  Single-line JSON on stdin (includes optional "device" field)
  Response: \x01 + header (6 fields: flag,samples,channels,sr,seed,timeMs) + float32 PCM
  Error:    \x00 + uint32 length + UTF-8 message
  Ready:    \x02 + uint16 length + JSON {"devices": [...], "default": "..."}

Loads one startup pipeline on the preferred device and lazy-loads additional
device/model combinations on demand.
Noise generation is patched to use numpy PCG64 for cross-platform determinism
(same seed → same audio on CPU, CUDA, ARM, x86).
"""

import base64
import json
import math
import os
import re
import struct
import sys
import time
import logging
import copy
import gc
from collections import OrderedDict
from pathlib import Path

# BEFORE ANYTHING HEAVY. The LRO's gates have to hear a body before it is called
# a finished instrument, and performing a Csound orchestra inside this process
# would put an authored endless loop or abort in the same address space as a
# multi-GB model. It goes in a child — and the only child available used to be an
# installed csound CLI, which no app bundle contains, so on a machine without
# Homebrew Csound nothing was ever heard and everything passed. This app is that
# missing binary: re-run with --lro-render, it performs one CSD through the
# CsoundLib64 the engine already carries and exits. Dispatched here, above the
# torch import, because the child must not pay for or touch a model.
#
# The flag comes FROM lco_write, it is not spelled again here: the two halves are
# a process boundary apart, so a literal that drifted would not fail a build or a
# test — this branch would simply not be taken, and the child would fall through
# into a second full backend that inherits the host's IPC pipe on stdin and eats
# the requests meant for the real one. lco_write is standard library only, so the
# import costs a normal launch nothing.
from lco_write import RENDER_FLAG, render_child_main  # noqa: E402

if len(sys.argv) > 1 and sys.argv[1] == RENDER_FLAG:
    sys.exit(render_child_main(sys.argv[2:]))


def _configure_cpu_budget_env():
    """Keep inference helper threads from starving the real-time audio process."""
    worker_threads = os.environ.get("T5YNTH_INFERENCE_CPU_THREADS", "2").strip()
    interop_threads = os.environ.get("T5YNTH_INFERENCE_INTEROP_THREADS", "1").strip()

    def positive_int_or_default(raw_value, default):
        try:
            value = int(raw_value)
        except (TypeError, ValueError):
            return default
        return max(1, value)

    worker_threads = str(positive_int_or_default(worker_threads, 2))
    interop_threads = str(positive_int_or_default(interop_threads, 1))

    def cap_thread_env(key, limit):
        raw_value = os.environ.get(key)
        try:
            current = int(raw_value) if raw_value is not None else None
        except ValueError:
            current = None

        if current is None or current < 1 or current > int(limit):
            os.environ[key] = limit

    for key in (
        "OMP_NUM_THREADS",
        "MKL_NUM_THREADS",
        "OPENBLAS_NUM_THREADS",
        "NUMEXPR_NUM_THREADS",
        "VECLIB_MAXIMUM_THREADS",
        "BLIS_NUM_THREADS",
        "TORCH_NUM_THREADS",
    ):
        cap_thread_env(key, worker_threads)

    cap_thread_env("TORCH_NUM_INTEROP_THREADS", interop_threads)
    os.environ.setdefault("OMP_WAIT_POLICY", "PASSIVE")
    os.environ.setdefault("KMP_BLOCKTIME", "0")
    os.environ.setdefault("MKL_DYNAMIC", "TRUE")


_configure_cpu_budget_env()

import numpy as np
import torch


def _apply_torch_cpu_budget():
    def env_int(name, default):
        try:
            return max(1, int(os.environ.get(name, str(default))))
        except ValueError:
            return default

    worker_threads = env_int("TORCH_NUM_THREADS", 2)
    interop_threads = env_int("TORCH_NUM_INTEROP_THREADS", 1)

    try:
        torch.set_num_threads(worker_threads)
        torch.set_num_interop_threads(interop_threads)
    except RuntimeError as exc:
        logging.getLogger("pipe_inference").warning(
            "Could not apply torch CPU budget after startup: %s", exc
        )


_apply_torch_cpu_budget()

# ─── Cross-platform deterministic noise ─────────────────────────────
# torch.Generator("cpu") and torch.Generator("cuda") are different PRNGs —
# same seed produces different sequences. torchsde's BrownianTree uses
# torch.Generator(device) internally (brownian_interval.py:31).
#
# Fix: monkey-patch torchsde._randn to use numpy PCG64 which is identical
# on all platforms (ARM, x86, any OS).

def _patch_torchsde_for_determinism():
    """Replace torchsde's device-dependent _randn with numpy PCG64."""
    try:
        import torchsde._brownian.brownian_interval as _bi

        def _deterministic_randn(size, dtype, device, seed):
            rng = np.random.Generator(np.random.PCG64(int(seed)))
            arr = rng.standard_normal(size).astype(np.float32)
            return torch.from_numpy(arr).to(dtype=dtype, device=device)

        _bi._randn = _deterministic_randn
        logging.getLogger("pipe_inference").info("torchsde patched for deterministic noise")
    except ImportError:
        pass  # torchsde not installed — patch not needed

_patch_torchsde_for_determinism()

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s",
                    stream=sys.stderr)
log = logging.getLogger("pipe_inference")

MAX_DIMENSION_OFFSET = 4.0
BLACKWELL_MIN_CUDA = (12, 8)

# Embedding-noise sigma is scaled down before being added to the conditioning
# tensor. The raw 0..1 slider value pushes the embedding off-manifold very
# quickly — at slider=0.1 the perceptual change is already extreme — so we
# halve the effective sigma to give the slider a usable resolution. The
# displayed/logged value still reflects what the user set; only the applied
# magnitude is reduced.
NOISE_SIGMA_SCALE = 0.5


def _sanitize_dimension_offsets(raw_offsets):
    """Validate and clamp sparse per-dimension offsets from the UI."""
    if not raw_offsets:
        return []

    cleaned = []
    dropped = 0
    clamped = 0

    for item in raw_offsets:
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            dropped += 1
            continue

        try:
            idx = int(item[0])
            value = float(item[1])
        except (TypeError, ValueError):
            dropped += 1
            continue

        if not math.isfinite(value):
            dropped += 1
            continue

        bounded = max(-MAX_DIMENSION_OFFSET, min(MAX_DIMENSION_OFFSET, value))
        if bounded != value:
            clamped += 1

        if abs(bounded) <= 1e-8:
            continue

        cleaned.append((idx, bounded))

    if dropped or clamped:
        log.warning(
            "Sanitized dimension offsets: kept=%d, clamped=%d, dropped=%d, limit=%.2f",
            len(cleaned), clamped, dropped, MAX_DIMENSION_OFFSET,
        )

    return cleaned


def _apply_dimension_offsets(tensor, dim_offsets):
    """Apply sparse additive offsets to the last tensor dimension."""
    if not dim_offsets:
        return

    last_dim = tensor.shape[-1]
    for idx, value in dim_offsets:
        if 0 <= idx < last_dim:
            tensor[:, :, idx] += value


def _parse_version_tuple(raw_value):
    """Parse '12.8' or '2.11.0.dev...' into a comparable integer tuple."""
    if not raw_value:
        return None

    parts = []
    for chunk in str(raw_value).split("."):
        digits = ""
        for char in chunk:
            if char.isdigit():
                digits += char
            else:
                break
        if not digits:
            break
        parts.append(int(digits))

    if not parts:
        return None
    if len(parts) == 1:
        parts.append(0)
    return tuple(parts[:2])


def _is_blackwell_capability(capability):
    """Treat sm_120+ GPUs as Blackwell-class devices."""
    return bool(capability) and capability[0] >= 12


def _validate_cuda_runtime_or_raise():
    """Fail closed when the bundled torch runtime is too old for the host GPU."""
    if os.environ.get("T5YNTH_ALLOW_INCOMPATIBLE_CUDA") == "1":
        return

    if not torch.cuda.is_available():
        return

    bundle_cuda_version = _parse_version_tuple(torch.version.cuda)
    incompatible_devices = []

    for device_index in range(torch.cuda.device_count()):
        capability = torch.cuda.get_device_capability(device_index)
        if not _is_blackwell_capability(capability):
            continue

        if bundle_cuda_version is None or bundle_cuda_version < BLACKWELL_MIN_CUDA:
            incompatible_devices.append(
                (
                    device_index,
                    torch.cuda.get_device_name(device_index),
                    f"sm_{capability[0]}{capability[1]}",
                )
            )

    if not incompatible_devices:
        return

    device_summary = ", ".join(
        f"GPU{device_index} {name} ({sm_name})"
        for device_index, name, sm_name in incompatible_devices
    )
    required_cuda = ".".join(str(part) for part in BLACKWELL_MIN_CUDA)
    raise RuntimeError(
        "Incompatible CUDA backend bundle: detected Blackwell GPU(s) "
        f"{device_summary}, but this backend was built with torch "
        f"{torch.__version__} / CUDA {torch.version.cuda or 'unknown'}. "
        f"Blackwell requires a bundle built with CUDA {required_cuda}+ "
        "(use the pinned Blackwell torch stack, not cu124)."
    )

# ─── Model loading ──────────────────────────────────────────────────

def _diffusers_weights_present(model_dir, idx):
    """True when a model_index.json is backed by weights that are actually there.

    diffusers keeps its weights in per-component SUBFOLDERS (unet/, vae/, ...),
    so the index file alone says nothing about whether the model is installed.
    A model slot is a symlink to the directory the model was imported from
    (SettingsPage::importModelDirectoryForId), and one pointing at a folder that
    merely happens to contain a leftover model_index.json — a browser Downloads
    folder, say — used to register as a complete engine: the plugin offered it,
    and the failure only surfaced at load time as "no file named
    diffusion_pytorch_model.bin found in directory ...". Check the components the
    index itself declares, so nothing scans an unrelated tree.
    """
    for key, value in idx.items():
        if key.startswith("_"):
            continue
        # A component entry is a ["module", "ClassName"] pair; anything else
        # (scalars, nested dicts) is metadata, not a weights folder.
        if not (isinstance(value, (list, tuple)) and len(value) == 2):
            continue
        component = model_dir / key
        if not component.is_dir():
            continue
        for pattern in ("*.safetensors", "*.bin", "*.ckpt", "*.pth"):
            if any(component.glob(pattern)):
                return True
    return False


def _model_format(model_dir):
    """Detect model format. Returns 'diffusers', 'audioldm2', 'native', or None."""
    native_weights = (model_dir / "model.safetensors").is_file()
    config_path = model_dir / "model_config.json"
    model_index = model_dir / "model_index.json"
    if model_index.is_file():
        try:
            with open(model_index) as f:
                idx = json.load(f)
        except (json.JSONDecodeError, OSError):
            idx = None
        if idx is not None and not _diffusers_weights_present(model_dir, idx):
            # Index without weights: not an install. Fall through to the native
            # check so a native model that happens to carry an index is still
            # found, and report nothing rather than a model that cannot load.
            if native_weights and config_path.is_file():
                return "native"
            log.warning("skipping %s: model_index.json without component weights",
                        model_dir)
            return None
        if idx is not None and "AudioLDM2" in idx.get("_class_name", ""):
            return "audioldm2"
        if native_weights and config_path.is_file():
            return "native"
        return "diffusers"
    if native_weights and config_path.is_file():
        return "native"
    return None

# {name: "diffusers"|"native"} — format of each discovered model
_model_formats = {}

def _model_search_base_dirs():
    """Return model root directories the backend scans for installed engines."""
    import sys, os
    configured_dir = os.environ.get("T5YNTH_MODEL_DIR")
    if configured_dir:
        return [Path(configured_dir)]

    base_dirs = [
        # CANONICAL per-user macOS root — this is the folder the app actually WRITES:
        # SettingsPage::getAppSupportModelDir() (src/gui/SetupWizard.cpp) resolves
        # JUCE's userApplicationDataDirectory, which on macOS is ~/Library, plus
        # "T5ynth/models". It used to be listed BELOW as "legacy" while the
        # Application Support path was labelled "primary" — but nothing ever writes
        # that one, so the writer and the reader disagreed about where models live
        # and the multi-root scan hid it.
        Path.home() / "Library" / "T5ynth" / "models",       # per-user macOS (canonical, app-written)
        Path.home() / "Library" / "Application Support" / "T5ynth" / "models",  # manual/older placement, scan-only
        Path.home() / ".config" / "share" / "T5ynth" / "models",  # Linux current app data path
        Path.home() / ".local" / "share" / "T5ynth" / "models",  # Linux
        Path.home() / "t5ynth" / "models",                    # legacy
    ]
    if sys.platform == "darwin":
        base_dirs.insert(1, Path("/Library/Application Support/T5ynth/models"))  # system-wide (.pkg, scan-only)
    elif sys.platform == "win32":
        base_dirs.insert(0, Path(os.environ.get("APPDATA", "")) / "T5ynth" / "models")  # per-user Windows (primary)
        base_dirs.insert(1, Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData")) / "T5ynth" / "models")  # system-wide (scan-only)
    return base_dirs


def find_models():
    """Discover all model directories (diffusers or native). Returns {name: Path}."""
    base_dirs = _model_search_base_dirs()
    models = {}
    for base in base_dirs:
        if not base.is_dir():
            continue
        for child in sorted(base.iterdir()):
            fmt = _model_format(child) if child.is_dir() else None
            if fmt and child.name not in models:
                models[child.name] = child
                _model_formats[child.name] = fmt
    return models


def _is_local_transformers_model_dir(path):
    """Return True if the directory looks like a self-contained HF model."""
    if not path.is_dir():
        return False

    has_config = (path / "config.json").is_file()
    # Accept both single-file and SHARDED checkpoints. Instruct models above
    # ~1 GB (e.g. the optional translation LLM, or larger SA3 encoders) ship
    # as `model.safetensors.index.json` + `model-0000N-of-...` shards and have
    # no single `model.safetensors`. Widening only ever accepts more dirs, so
    # existing single-file models (t5-base, t5gemma-b-b) still qualify.
    has_weights = any(
        (path / name).is_file()
        for name in (
            "model.safetensors",
            "model.safetensors.index.json",
            "pytorch_model.bin",
            "pytorch_model.bin.index.json",
        )
    )
    has_tokenizer = any(
        (path / name).is_file()
        for name in ("tokenizer.json", "spiece.model")
    )
    return has_config and has_weights and has_tokenizer


def _bundled_transformers_asset_roots():
    """Return PyInstaller/source roots that can contain bundled HF assets."""
    roots = []

    explicit_asset_root = os.environ.get("T5YNTH_BACKEND_HF_ASSETS")
    if explicit_asset_root:
        roots.append(Path(explicit_asset_root))

    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        roots.append(Path(meipass))

    if getattr(sys, "frozen", False):
        exe_dir = Path(sys.executable).resolve().parent
        roots.extend([exe_dir, exe_dir / "_internal"])

    module_dir = Path(__file__).resolve().parent
    roots.extend([module_dir, module_dir / "_internal"])

    seen = set()
    for root in roots:
        resolved = root.expanduser()
        if resolved in seen:
            continue
        seen.add(resolved)
        yield resolved


def _candidate_transformers_dirs(model_name, model_dir):
    """Yield asset locations for auxiliary HF transformer encoders (e.g. t5-base).

    Searches user-installed roots first (peer to the audio-model directories,
    covering both the active and legacy T5ynth model roots so a conditioner
    installed in any of them is found regardless of where the audio model
    lives), then bundled asset locations as a fallback for development builds
    where backend/hf_assets/ is still present.
    """
    short_name = model_name.rsplit("/", 1)[-1]
    canonical_name = model_name.replace("/", "--")

    seen = set()

    user_roots = []
    if model_dir is not None:
        user_roots.append(Path(model_dir).expanduser().parent)
    for base in _model_search_base_dirs():
        user_roots.append(Path(base).expanduser())

    for root in user_roots:
        for rel in (short_name, canonical_name):
            candidate = (root / rel).expanduser()
            if candidate in seen:
                continue
            seen.add(candidate)
            yield candidate

    bundled_candidates = [
        Path("hf_assets") / short_name,
        Path("hf_assets") / canonical_name,
    ]

    for root in _bundled_transformers_asset_roots():
        for rel in bundled_candidates:
            candidate = (root / rel).expanduser()
            if candidate in seen:
                continue
            seen.add(candidate)
            yield candidate


def _resolve_local_transformers_model_dir(model_name, model_dir):
    """Resolve a local HF asset directory for a model id like t5-base.

    Prefers user-installed assets (via SetupWizard) over bundled ones.
    """
    for candidate in _candidate_transformers_dirs(model_name, model_dir):
        if _is_local_transformers_model_dir(candidate):
            return candidate
    return None


def _prepare_native_model_config(model_dir, model_config):
    """Rewrite native model config to point at local auxiliary HF assets."""
    patched = copy.deepcopy(model_config)
    conditioning_cfg = patched.get("model", {}).get("conditioning", {})
    configs = conditioning_cfg.get("configs", [])
    resolved_t5_models = []

    for cond in configs:
        if cond.get("type") != "t5":
            continue

        cond_cfg = cond.setdefault("config", {})
        model_name = cond_cfg.get("t5_model_name")
        if not model_name:
            continue

        resolved = _resolve_local_transformers_model_dir(model_name, model_dir)
        if resolved is None:
            searched = [str(path) for path in _candidate_transformers_dirs(model_name, model_dir)]
            expected = str((Path(model_dir).expanduser().parent / model_name.rsplit("/", 1)[-1]))
            raise RuntimeError(
                "Native Stable Audio model files were found, but the required "
                f"text encoder is not installed: '{model_name}'. Place a "
                f"self-contained directory at '{expected}' (e.g. via "
                f"'huggingface-cli download {model_name} --local-dir {expected}'). "
                f"Searched: {searched}"
            )

        cond_cfg["t5_model_name"] = str(resolved)
        log.info("Resolved transformers asset %s -> %s", model_name, resolved)
        resolved_t5_models.append((model_name, str(resolved)))

    # SA3 family: the t5gemma text encoder ships as a subfolder INSIDE the
    # model dir (e.g. <model_dir>/t5gemma-b-b-ul2/). Rewrite the conditioner to
    # load it locally+offline via model_path instead of pulling repo_id over the
    # network. T5GemmaConditioner.load_from = model_path or repo_id or model_name,
    # so model_path wins; no T5 registry patch is needed for t5gemma.
    for cond in configs:
        if cond.get("type") != "t5gemma":
            continue
        cond_cfg = cond.setdefault("config", {})
        subfolder = cond_cfg.get("subfolder") or "t5gemma-b-b-ul2"
        encoder_dir = Path(model_dir) / subfolder
        if not (encoder_dir / "model.safetensors").is_file():
            raise RuntimeError(
                "Native Stable Audio 3 model files were found, but the t5gemma "
                f"text encoder subfolder is missing or incomplete: '{encoder_dir}'. "
                f"Install the '{subfolder}' files from the model's HF repo into the "
                "model directory."
            )
        cond_cfg.pop("repo_id", None)
        cond_cfg["model_path"] = str(Path(model_dir))
        cond_cfg["subfolder"] = subfolder
        log.info("Resolved t5gemma encoder -> %s", encoder_dir)

    return patched, resolved_t5_models


def _patch_stable_audio_tools_t5_registry(resolved_t5_models):
    """Allow stable-audio-tools to accept resolved bundled T5 asset directories.

    The patched native model config rewrites ``t5_model_name`` from the
    repo-style name (e.g. ``t5-base``) to the absolute path of the bundled
    asset. T5Conditioner.__init__ then asserts the resolved path is present
    in its T5_MODEL_DIMS / T5_MODELS registries — neither of which knows
    about local paths out of the box. We copy the registry entry for the
    original name to the resolved path so the assert passes.
    """
    if not resolved_t5_models:
        return

    from stable_audio_tools.models import conditioners as sat_conditioners

    for original_name, resolved_path in resolved_t5_models:
        dims = getattr(sat_conditioners.T5Conditioner, "T5_MODEL_DIMS", None)
        models = getattr(sat_conditioners.T5Conditioner, "T5_MODELS", None)
        if not isinstance(dims, dict):
            continue

        if isinstance(models, tuple):
            models = list(models)
            sat_conditioners.T5Conditioner.T5_MODELS = models
        if isinstance(models, list) and resolved_path not in models:
            models.append(resolved_path)
        if isinstance(models, list) and original_name not in models:
            models.append(original_name)

        if original_name in dims:
            dims[resolved_path] = dims[original_name]


def _native_pingpong_supported():
    """Return True iff the bundled stable-audio-tools can run pingpong.

    The SA3 family ships with ``sampler_type='pingpong'`` per its HF model
    cards. The pinned build (``stable-audio-tools==0.0.19``) implements it
    via a dedicated ``sample_flow_pingpong`` function in
    ``inference/sampling.py`` (and also keeps the legacy ``sample_rf``
    dispatch whose whitelist includes 'pingpong').

    Used by ``_generate_native`` to raise a precise error when an SA3
    model is loaded against a library that lacks pingpong — without that
    check the failure surfaces as ``'NoneType' object has no attribute
    'to'`` from inside ``generate_diffusion_cond``, the same opaque
    symptom any other unrecognised sampler_type would give.
    """
    try:
        from stable_audio_tools.inference import sampling as sat_sampling
        # Prefer the dedicated ``sample_flow_pingpong`` function (present in
        # 0.0.19 and 0.0.20). Fall back to the legacy ``sample_rf`` whitelist
        # probe. NB: 0.0.20 removed ``sample_rf`` entirely and only exposes
        # ``sample_flow_pingpong`` — probing ``sample_rf`` alone (as an
        # earlier version of this function did) raised AttributeError there
        # and wrongly reported pingpong unsupported, blocking SA3 generation.
        # The hasattr check is robust across both library versions.
        if hasattr(sat_sampling, "sample_flow_pingpong"):
            return True
        sample_rf = getattr(sat_sampling, "sample_rf", None)
        return sample_rf is not None and "pingpong" in sample_rf.__code__.co_consts
    except Exception:
        return False


_UNSUPPORTED_SAMPLER_MSG = (
    "Model {model!r} requested sampler_type={sampler!r}, which the bundled "
    "stable-audio-tools build does not recognise. Without a matching branch "
    "in sample_rf / sample_k, generate_diffusion_cond silently returns None "
    "and trips an opaque AttributeError downstream. Upgrade stable-audio-tools "
    "or remove the sampler_type override for this model."
)


def _patch_apg_for_mps():
    """git-main stable-audio-tools applies APG (Adaptive Projected Guidance) in
    the DiT forward for ALL diffusion_cond(_inpaint) models, and ``apg_project``
    casts its working tensors to float64 — which the MPS backend cannot do
    ("Cannot convert a MPS Tensor to float64"). Replace ``apg_project`` with a
    version that works in float32 on MPS (math identical; guidance precision loss
    is negligible for audio) and keeps float64 on CPU/CUDA so those paths are
    bit-unchanged. Idempotent and a no-op on builds without ``apg_project``
    (e.g. the legacy 0.0.19 pin), so it is safe to call on every native load.
    """
    try:
        import torch
        from stable_audio_tools.models.dit import DiffusionTransformer
    except Exception:
        return
    if getattr(DiffusionTransformer, "_t5ynth_apg_mps_patched", False):
        return
    if not hasattr(DiffusionTransformer, "apg_project"):
        return

    def apg_project(self, v0, v1, padding_mask=None):
        dtype = v0.dtype
        work = torch.float32 if v0.device.type == "mps" else torch.float64
        v0, v1 = v0.to(work), v1.to(work)
        if padding_mask is not None:
            mask = padding_mask.unsqueeze(1).to(work)
            v0m, v1m = v0 * mask, v1 * mask
            v1u = v1m / v1m.norm(dim=[-1, -2], keepdim=True).clamp(min=1e-8)
            v0_parallel = (v0m * v1u).sum(dim=[-1, -2], keepdim=True) * v1u
            v0_orthogonal = (v0 - (v0 * v1u).sum(dim=[-1, -2], keepdim=True) * v1u) * mask
        else:
            v1 = torch.nn.functional.normalize(v1, dim=[-1, -2])
            v0_parallel = (v0 * v1).sum(dim=[-1, -2], keepdim=True) * v1
            v0_orthogonal = v0 - v0_parallel
        return v0_parallel.to(dtype), v0_orthogonal.to(dtype)

    DiffusionTransformer.apg_project = apg_project
    DiffusionTransformer._t5ynth_apg_mps_patched = True
    log.info("Patched DiT.apg_project for MPS (float32).")


def _force_hf_offline_for_native_load():
    """Prevent transformers/huggingface_hub from falling back to network I/O."""
    keys = ("HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE")
    previous = {key: os.environ.get(key) for key in keys}
    for key in keys:
        os.environ[key] = "1"
    return previous


def _restore_hf_env(previous):
    for key, value in previous.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value


# ─── Memory-budgeted pipeline cache (per device-pool LRU) ─────────────
#
# Pipelines are cached by (model, device) and reused across requests, but the
# cache is BOUNDED, not unbounded: before loading a new pipeline, the least-
# recently-used pipelines sharing the same memory pool are evicted (and their
# device memory reclaimed) until the incoming model is predicted to fit a
# budget. Without this, switching models keeps every model ever touched
# resident at once — fatal on unified-memory Macs (MPS + CPU share one
# 8-16 GB pool) where two fp32 models exceed RAM and the backend gets OOM-killed.
#
# Pools are device-aware because VRAM and host RAM are SEPARATE on discrete
# GPUs: a CUDA pipeline occupies that GPU's VRAM, not system RAM, so it is
# weighed against — and only ever evicts — other residents of the same VRAM
# pool (live figure from torch.cuda.mem_get_info). On Apple Silicon, MPS and
# CPU share one unified "system" pool sized from total physical RAM.
_available_models = {}              # {name: Path}  — set in main()
_loaded_pipelines = OrderedDict()   # {(model, device): pipeline}, LRU oldest-first
_pipeline_bytes = {}                # {(model, device): resident bytes (measured/estimated)}


def _env_float(name, default):
    try:
        value = float(os.environ.get(name, ""))
        return value if value > 0 else default
    except (TypeError, ValueError):
        return default


# Fraction of total system RAM the unified/system pool may hold. Conservative:
# the host DAW, the JUCE plugin and the OS draw from the same RAM.
_SYSTEM_MEM_FRACTION = _env_float("T5YNTH_MEM_FRACTION", 0.70)
# Multiplier on an incoming model's on-disk weight size to cover runtime
# scratch (activations, latents, sampler state) not present on disk.
_MEM_HEADROOM = 1.15
_WEIGHT_SUFFIXES = (".safetensors", ".bin", ".ckpt", ".pt", ".pth")
# When a model's on-disk size can't be read, assume it is large so the pool is
# evicted conservatively rather than a model loaded blindly on top of residents.
_DEFAULT_UNKNOWN_BYTES = 8 * 1024 ** 3


def _total_system_ram():
    """Total physical RAM in bytes, or 0 if undeterminable (→ single-resident)."""
    try:
        return os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (ValueError, OSError, AttributeError):
        pass
    if sys.platform == "win32":
        try:
            import ctypes

            class _MS(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_uint32), ("dwMemoryLoad", ctypes.c_uint32),
                            ("ullTotalPhys", ctypes.c_uint64), ("ullAvailPhys", ctypes.c_uint64),
                            ("ullTotalPageFile", ctypes.c_uint64), ("ullAvailPageFile", ctypes.c_uint64),
                            ("ullTotalVirtual", ctypes.c_uint64), ("ullAvailVirtual", ctypes.c_uint64),
                            ("ullAvailExtendedVirtual", ctypes.c_uint64)]

            status = _MS()
            status.dwLength = ctypes.sizeof(_MS)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
            return int(status.ullTotalPhys)
        except Exception:
            pass
    return 0


def _device_pool(device):
    """Memory pool a device draws from. Discrete-GPU VRAM is its own pool;
    MPS shares the unified 'system' pool with the CPU."""
    if isinstance(device, str) and device.startswith("cuda"):
        return device if ":" in device else "cuda:0"
    return "system"


def _estimate_model_bytes(model_dir):
    """A-priori resident size: total bytes of weight files under model_dir."""
    total = 0
    try:
        for path in Path(model_dir).rglob("*"):
            if path.suffix.lower() in _WEIGHT_SUFFIXES and path.is_file():
                total += path.stat().st_size
    except OSError:
        pass
    return total


def _incoming_bytes(model_dir):
    """Pre-load size estimate, never 0 (a 0 would disable the budget check)."""
    return _estimate_model_bytes(model_dir) or _DEFAULT_UNKNOWN_BYTES


def _system_pool_used():
    """Bytes held by the unified/system pool. Uses the LARGER of (a) the sum of
    the cache's own tracked pipeline sizes — which drops deterministically as we
    evict, so the eviction loop always makes progress — and (b) the live Metal-
    driver figure, a ground-truth floor that also catches scratch, fragmentation
    and the resident translator that the tracked sum omits. The driver figure is
    only meaningful after empty_cache (it otherwise counts retained-but-free
    blocks), which _make_room calls before every measurement."""
    tracked = sum(b for k, b in _pipeline_bytes.items()
                  if k in _loaded_pipelines and _device_pool(k[1]) == "system")
    if torch.backends.mps.is_available():
        return max(tracked, torch.mps.driver_allocated_memory())
    return tracked


def _can_fit(pool, need):
    """True if `need` bytes can be loaded into `pool` within budget."""
    want = need * _MEM_HEADROOM
    if pool.startswith("cuda"):
        try:
            index = int(pool.split(":")[1])
            free, _total = torch.cuda.mem_get_info(index)
        except Exception:
            return True  # cannot introspect VRAM → do not block the load
        return free >= want
    budget = _total_system_ram() * _SYSTEM_MEM_FRACTION
    if budget <= 0:
        return False  # unknown RAM → force eviction of all other residents
    return _system_pool_used() + want <= budget


def _reclaim(device):
    """Return freed device memory to the allocator after dropping references."""
    gc.collect()
    if isinstance(device, str) and device.startswith("cuda") and torch.cuda.is_available():
        torch.cuda.empty_cache()
    elif device == "mps" and torch.backends.mps.is_available():
        torch.mps.empty_cache()
        torch.mps.synchronize()


def _evict_translators(pool):
    """Drop cached prompt translators in `pool` — last resort before giving up
    on freeing space for an audio model."""
    for tkey in list(_translator_cache.keys()):
        if _device_pool(tkey[1]) == pool:
            _translator_cache.pop(tkey, None)
            log.info(f"Evicting prompt translator on {tkey[1]} to free {pool} memory")
            _reclaim(tkey[1])


def _make_room(pool, device, need):
    """Evict least-recently-used residents of `pool` until `need` bytes fit, or
    nothing more in the pool can be evicted (then the caller loads anyway — a
    single model on an otherwise-empty pool is the best that can be done)."""
    _reclaim(device)  # drop retained-but-free blocks so the first reading is live
    while not _can_fit(pool, need):
        victim = next((k for k in _loaded_pipelines if _device_pool(k[1]) == pool), None)
        if victim is None:
            break
        pipe = _loaded_pipelines.pop(victim)
        _pipeline_bytes.pop(victim, None)
        log.info(f"Evicting {victim[0]} on {victim[1]} to free {pool} memory "
                 f"(need ~{need / 1e9:.1f} GB)")
        del pipe
        _reclaim(victim[1])
    if not _can_fit(pool, need):
        _evict_translators(pool)  # last resort: the translator shares this pool too


def get_pipeline(model_name, device):
    """Get or lazily load a pipeline for model+device. New loads first evict
    LRU pipelines in the same memory pool so resident models stay in budget."""
    key = (model_name, device)
    cached = _loaded_pipelines.get(key)
    if cached is not None:
        _loaded_pipelines.move_to_end(key)  # mark most-recently-used
        return cached

    if model_name not in _available_models:
        raise ValueError(f"Unknown model: {model_name} (have: {list(_available_models.keys())})")
    model_dir = _available_models[model_name]

    need = _incoming_bytes(model_dir)
    _make_room(_device_pool(device), device, need)

    log.info(f"Lazy-loading {model_name} on {device}...")
    on_mps = device == "mps" and torch.backends.mps.is_available()
    used_before = torch.mps.driver_allocated_memory() if on_mps else 0
    pipe = load_pipeline(model_dir, device)
    if on_mps:
        # Reclaim load scratch before measuring so the tracked size is the
        # model's live footprint, not transient buffers empty_cache would free.
        gc.collect()
        torch.mps.empty_cache()
        torch.mps.synchronize()
        _pipeline_bytes[key] = max(torch.mps.driver_allocated_memory() - used_before, need)
    else:
        _pipeline_bytes[key] = need
    _loaded_pipelines[key] = pipe  # inserted last = most-recently-used
    return pipe


def available_devices():
    """Return list of available inference devices, best first."""
    devices = []
    if torch.backends.mps.is_available():
        devices.append("mps")
    if torch.cuda.is_available():
        _validate_cuda_runtime_or_raise()
        devices.append("cuda")
    devices.append("cpu")
    return devices


# ─── Optional prompt translation (lazy, deterministic) ────────────────
#
# An opt-in, user-downloaded small instruct LLM translates non-English
# prompts to English before conditioning. The audio models (SAO/SA3) are
# trained on English captions, so non-English prompts underperform. The
# translation is a pure, EPHEMERAL pre-step: the JUCE client keeps the
# user's ORIGINAL text in the prompt fields and never persists the English
# (so recursive prompt editing stays in the user's own language). It is
# invoked once per prompt edit and cached client-side.
#
# Greedy decoding (num_beams=1, do_sample=False) for determinism within a
# device — the same source text yields the same English on a given backend.

TRANSLATION_SYSTEM_PROMPT = (
    "You are a translation engine for short text-to-audio generation prompts. "
    "Translate the user's text into English. "
    "Output ONLY the English translation: no quotes, no notes, no explanation, no preamble. "
    "Preserve musical instrument names, genre names and proper nouns. "
    "If the text is already English, return it unchanged."
)

_translator_cache = {}  # {(model_dir_str, device): (tokenizer, model)}


# The LCO author ships as a single 4-bit GGUF (google/gemma-4-12B-it-qat-q4_0-gguf),
# not a transformers directory: a 12B in 6.98 GB, which is what makes it fit a
# 16 GB Mac at all -- the same model in bf16 is 23.9 GB, and transformers has no
# working 4-bit path on MPS (bitsandbytes is CUDA-only). llama.cpp runs the same
# file on Metal and on CUDA.
_MMPROJ_PREFIX = "mmproj"
# Context the author is loaded with. A KV-cache size, NOT an output cap: an
# orchestra is a few hundred tokens, so this never truncates one, and generation
# itself runs uncapped to EOS (run_gguf_instruct). Overridable for a machine that
# wants a smaller cache.
_GGUF_N_CTX = int(os.environ.get("T5YNTH_CODER_N_CTX", 65536))
# The install slot the Model Manager writes and the resolver checks by name,
# so a leftover coder directory cannot shadow it in the alphabetical scan.
_CODER_SLOT = "gemma-4-12b-it-qat-q4_0"


def _coder_gguf_file(path):
    """The single text GGUF inside a coder slot, or None.

    Ignores the `mmproj*` multimodal projector Google ships beside it: that file
    is the vision/audio tower, not a language model, and loading it as one fails.
    A slot with more than one text GGUF is ambiguous, so it is refused rather than
    silently picking one.
    """
    if not path.is_dir():
        return None
    ggufs = [f for f in sorted(path.glob("*.gguf"))
             if not f.name.lower().startswith(_MMPROJ_PREFIX)]
    return ggufs[0] if len(ggufs) == 1 else None


def _is_local_coder_model_dir(path):
    """A usable LCO author slot: a GGUF the llama.cpp path can open, or a
    self-contained transformers directory (the pre-GGUF install shape, and any
    dev drop)."""
    return _coder_gguf_file(path) is not None or _is_local_transformers_model_dir(path)


def _resolve_coder_model_dir(request):
    """Locate the LCO coder model directory (a HF causal-LM dir), or None.

    The ONE instruct model in the product: it authors the Csound, translates
    prompts and drives Re-Prompt. The separately-installed 1.5B translator it
    replaced is gone (it was measurably bad at both of its jobs).

    Precedence:
      1. request["coder_model_path"]              — explicit path from the client
      2. $T5YNTH_CODER_MODEL / $LCO_MODEL_DIR     — override (dev/testing)
      3. <model root>/coder/<_CODER_SLOT>         — the authoritative install slot
                                                    (checked by EXACT name, so it
                                                    wins even if an older coder dir
                                                    also exists)
      4. <model root>/coder/<dir>                 — auto-discovered installed model
         <model root>/lco-coder/<dir>                (dev/legacy drop)
    The retired "LCO interpreter" (interpret/qwen2.5-7b-instruct) is NOT resolved:
    it is this model or nothing (LLM-first, no fallback).
    """
    explicit = (request.get("coder_model_path")
                or os.environ.get("T5YNTH_CODER_MODEL")
                or os.environ.get("LCO_MODEL_DIR"))
    if explicit:
        candidate = Path(explicit).expanduser()
        return candidate if _is_local_coder_model_dir(candidate) else None

    # Check the exact, current install slot FIRST and BY NAME rather than folding
    # it into the sorted directory scan below: that scan sorts alphabetically, so a
    # leftover model from a previous install can sort BEFORE (and shadow) the one
    # the user actually installed -- that is how a stale 3B silently took over
    # authoring for an afternoon. Checking the authoritative name explicitly makes
    # the current model win regardless of whatever else is sitting in coder/. The
    # env overrides above stay supreme (a deployment that deliberately pins a model
    # still wins).
    for base in _model_search_base_dirs():
        authoritative = base / "coder" / _CODER_SLOT
        if _is_local_coder_model_dir(authoritative):
            return authoritative

    for base in _model_search_base_dirs():
        for sub in ("coder", "lco-coder"):
            coder_dir = base / sub
            if not coder_dir.is_dir():
                continue
            for child in sorted(coder_dir.iterdir()):
                if _is_local_coder_model_dir(child):
                    return child

    # No fallback beyond the coder dirs above: the retired "LCO interpreter"
    # (interpret/qwen2.5-7b-instruct) and the retired 1.5B translator are
    # deliberately NOT resolved — it is this model or nothing (LLM-first). A
    # missing model returns None and the caller raises the standard error frame.
    return None


_gguf_cache = {}  # {gguf_path_str: Llama}

# The author's attention is mostly SLIDING-WINDOW: 40 of Gemma's 48 layers look
# back 1,024 tokens and no further. llama-cpp-python still asks llama.cpp for a
# full-size cache on those layers (`swa_full=True` is its default), which at a
# 65,536-token window is 20.5 GB of GPU memory holding cells no layer can attend
# to -- measured 2026-11-28 on a 32 GB card: 6.6 GB model + 20.5 GB + 4.1 GB for
# the 8 global layers + 2.4 GB compute, and the context is refused. With the
# cache at the size the layers can use, the same window loads in 13.5 GB.
#
# What the full-size cache bought was the ROLLBACK: every request shares the
# system prompt with the one before it, and `Llama.generate` reuses that prefix
# by removing the cells after it. On a pruned sliding-window cache the removal
# still reports success, but the window in front of the prefix is gone, and the
# reply is computed without it -- measured, the logits after such a rollback
# differ by up to 32 and share none of the top ten tokens with the true ones.
# `_SwaCheckpoints` keeps the reuse and makes it correct. Set to 1 to load the
# full-size cache instead, as before.
_GGUF_SWA_FULL = os.environ.get("T5YNTH_CODER_SWA_FULL") == "1"
# Sliding-window states kept on the host, about 336 MB each (the window plus one
# batch, on 40 layers; independent of the context size). Not below 2: the
# eviction always keeps the shallowest and needs another one to let go.
#
# Verified on CUDA only, where llama.cpp runs this model without flash
# attention. On Metal flash attention is normally on and the state is saved and
# restored through llama.cpp's other code path; that path is NOT measured here.
# On a Mac the four states also come out of the same unified memory as the
# model. `T5YNTH_CODER_SWA_FULL=1` is the way back if either matters.
_SWA_CHECKPOINTS = 4


class _SwaCheckpoints:
    """Prefix reuse for a model whose sliding-window cache is not full-size.

    Wraps `llm.generate`. Before a prompt is handed on, the cache is brought to a
    state that is exactly what evaluating the shared prefix would have produced:

    - no rollback, or one short enough that the window in front of it is still
      in the cache (llama.cpp's own test: `seq_pos_min <= prefix - n_swa`):
      nothing to restore;
    - otherwise the sliding-window layers are restored from the deepest saved
      state that lies on this prompt, and the rest is evaluated from there. The
      global layers need no restore: their cells are never pruned;
    - no such state: everything is evaluated again.

    States are saved where a rollback lands: at the point a prompt left the
    cached one when that cost an evaluation, and at the end of the prompt, which
    is where the next turn of the same conversation picks up. A state is not
    saved within one window of an earlier one on the same prompt -- from there
    the earlier one is a restore and at most `n_swa` tokens away. Nothing here
    knows the chat template: the place the system prompt ends is found the first
    time two prompts part there."""

    def __init__(self, llm):
        import llama_cpp
        self._c = llama_cpp
        self._llm = llm
        self._generate = llm.generate
        self._n_swa = int(llama_cpp.llama_model_n_swa(llm._model.model))
        self._flags = llama_cpp.LLAMA_STATE_SEQ_FLAGS_SWA_ONLY
        self._saved = []   # oldest first: {"toks": tokens up to the position, "buf", "hits"}

    def generate(self, tokens, *args, **kw):
        if not args and kw.get("reset", True):
            tokens = list(tokens)
            try:
                self._prepare(tokens)
            except Exception as exc:
                # Evaluating everything again is always correct.
                log.warning(f"author cache: prefix reuse failed, re-evaluating: {exc}")
                self._saved.clear()
                self._drop_all()
        return self._generate(tokens, *args, **kw)

    def _drop_all(self):
        self._llm.reset()
        self._llm._ctx.kv_cache_seq_rm(-1, 0, -1)

    def _prepare(self, tokens):
        llm = self._llm
        end = len(tokens) - 1      # the last prompt token is generate()'s to evaluate
        if end < 1:
            return
        toks = np.asarray(tokens, dtype=np.intc)
        have = int(llm.n_tokens)
        m = min(have, len(toks))
        apart = np.nonzero(np.asarray(llm.input_ids[:m]) != toks[:m])[0]
        keep = min(int(apart[0]) if apart.size else m, end)
        marks = {end}
        if keep < have:            # the cache runs on past what this prompt shares
            pos_min = int(self._c.llama_memory_seq_pos_min(llm._ctx.memory, 0))
            if keep > 0 and 0 <= pos_min <= max(0, keep - self._n_swa):
                llm._ctx.kv_cache_seq_rm(-1, keep, -1)
                llm.n_tokens = keep
                llm._requires_eval = True
            else:
                self._restore(toks, keep)
                marks.add(keep)
        for pos in sorted(marks):
            if pos < llm.n_tokens:
                continue
            if pos > llm.n_tokens:
                llm.eval(tokens[llm.n_tokens:pos])
            self._save(toks, pos)

    def _deepest(self, toks, pos):
        """Index of the deepest saved state on this prompt at or before `pos`."""
        best = None
        for i, ck in enumerate(self._saved):
            n = len(ck["toks"])
            if (n <= pos and (best is None or n > len(self._saved[best]["toks"]))
                    and np.array_equal(ck["toks"], toks[:n])):
                best = i
        return best

    def _restore(self, toks, keep):
        llm = self._llm
        best = self._deepest(toks, keep)
        if best is not None:
            ck = self._saved[best]
            got = self._c.llama_state_seq_set_data_ext(
                llm._ctx.ctx, ck["buf"], len(ck["buf"]), 0, self._flags)
            if got == len(ck["buf"]):
                llm._ctx.kv_cache_seq_rm(-1, len(ck["toks"]), -1)
                llm.n_tokens = len(ck["toks"])
                llm._requires_eval = True
                ck["hits"] += 1
                return
            del self._saved[best]
        self._drop_all()

    def _save(self, toks, pos):
        import ctypes
        near = self._deepest(toks, pos)
        if pos - (len(self._saved[near]["toks"]) if near is not None else 0) < self._n_swa:
            return
        ctx = self._llm._ctx.ctx
        size = int(self._c.llama_state_seq_get_size_ext(ctx, 0, self._flags))
        buf = (ctypes.c_uint8 * size)()
        if int(self._c.llama_state_seq_get_data_ext(ctx, buf, size, 0, self._flags)) != size:
            return
        # Over the limit one goes, but never the SHALLOWEST: that one is shared
        # by every later prompt (the system prompt's end). Of the others, the
        # one restored from least often, the oldest first -- the end of a turn
        # that nothing came back to.
        if len(self._saved) >= _SWA_CHECKPOINTS:
            shallow = min(range(len(self._saved)), key=lambda i: len(self._saved[i]["toks"]))
            rest = [i for i in range(len(self._saved)) if i != shallow]
            del self._saved[min(rest, key=lambda i: self._saved[i]["hits"])]
        self._saved.append({"toks": toks[:pos].copy(), "buf": buf, "hits": 0})


def _get_gguf(gguf_path):
    """Lazily load + cache a llama.cpp model. All layers go to the GPU (Metal on
    macOS, CUDA elsewhere); llama.cpp falls back to CPU for what does not fit."""
    key = str(gguf_path)
    cached = _gguf_cache.get(key)
    if cached is not None:
        return cached
    from llama_cpp import Llama

    log.info(f"Loading LCO author (GGUF) from {gguf_path}...")
    real_stdout = sys.stdout
    sys.stdout = sys.stderr  # protect the IPC pipe from llama.cpp's own stdout
    try:
        llm = Llama(model_path=key, n_gpu_layers=-1, n_ctx=_GGUF_N_CTX,
                    swa_full=_GGUF_SWA_FULL, verbose=False)
    finally:
        sys.stdout = real_stdout
    if not _GGUF_SWA_FULL:
        import llama_cpp
        if llama_cpp.llama_model_n_swa(llm._model.model) > 0:
            llm.generate = _SwaCheckpoints(llm).generate
    _gguf_cache[key] = llm
    log.info("LCO author ready.")
    return llm


def run_gguf_instruct(text, gguf_path, system_prompt, max_new_tokens=None,
                      on_delta=None):
    """run_instruct's contract on the llama.cpp path: one short user text under an
    arbitrary system prompt, greedy for determinism, the model's own chat template.

    No invented output cap. `max_tokens=-1` means "until EOS or the context ends",
    which is the model's real ceiling; a number here would cut an orchestra off
    mid-line. Callers that pass max_new_tokens are honoured, but nothing in the
    csound path does.

    `on_delta(piece)` turns the call into a streaming one and is handed each
    fragment as it is decoded. The return value is identical either way — the
    caller still gets the whole reply — so nothing downstream has to know whether
    anyone was watching. A callback that raises is logged and dropped: whoever is
    listening must not be able to break the authoring.

    `text` may also be a LIST of chat messages instead of a single string, which
    is how the LCO repairs an orchestra: the author is handed its own failed
    answer plus the compiler's complaint and edits it, rather than being asked
    for the whole sound again from nothing. Sharing the leading turns also lets
    llama.cpp reuse the KV cache it already built for them."""
    if isinstance(text, (list, tuple)):
        turns = [dict(m) for m in text if (m.get("content") or "").strip()]
        if not turns:
            return ""
    else:
        text = (text or "").strip()
        if not text:
            return ""
        turns = [{"role": "user", "content": text}]
    llm = _get_gguf(gguf_path)
    kw = dict(messages=[{"role": "system", "content": system_prompt}] + turns,
              temperature=0.0,
              max_tokens=int(max_new_tokens) if max_new_tokens else -1)
    if on_delta is None:
        out = llm.create_chat_completion(**kw)
        return out["choices"][0]["message"]["content"] or ""

    parts = []
    for chunk in llm.create_chat_completion(stream=True, **kw):
        try:
            piece = (chunk["choices"][0].get("delta") or {}).get("content") or ""
        except (KeyError, IndexError, TypeError):
            continue
        if not piece:
            continue
        parts.append(piece)
        if on_delta is not None:
            try:
                on_delta(piece)
            except Exception as exc:
                log.warning(f"streaming callback failed, generation continues: {exc}")
                on_delta = None
    return "".join(parts)


# Qwen serves BOTH translate and interpret/reprompt — one shared instance. It
# normally runs on the audio device, but on a small-VRAM CUDA card co-residency
# with the audio model doesn't fit, so the per-pool LRU would swap them in and
# out across every reprompt→generate boundary (they're never active at the same
# instant, but each load evicts the other). The instruct step runs once per
# generation — not per audio block — so paying CPU latency there is tolerable,
# and it permanently frees ~2.9 GB of VRAM for the audio model. Large cards keep
# the translator on the GPU for speed. Mirrors CLAP's existing CPU pin.
# Override: T5YNTH_TRANSLATOR_CPU=1 forces CPU, T5YNTH_TRANSLATOR_GPU=1 forces GPU.
TRANSLATOR_GPU_MIN_VRAM = 7.0 * 1024 ** 3  # CUDA cards below this pin Qwen to CPU


def _translator_device(audio_device):
    """Device the instruct LLM (translate/interpret) should run on, given the
    audio device. CPU on small CUDA cards; unchanged on mps/cpu/large CUDA."""
    if not (isinstance(audio_device, str) and audio_device.startswith("cuda")):
        return audio_device  # mps/cpu: no separate VRAM pool to protect
    if os.environ.get("T5YNTH_TRANSLATOR_CPU") == "1":
        return "cpu"
    if os.environ.get("T5YNTH_TRANSLATOR_GPU") == "1":
        return audio_device
    try:
        idx = int(audio_device.split(":")[1]) if ":" in audio_device else torch.cuda.current_device()
        total = torch.cuda.get_device_properties(idx).total_memory
    except Exception:
        return audio_device  # can't measure → keep current (GPU) behavior
    return "cpu" if total < TRANSLATOR_GPU_MIN_VRAM else audio_device


def _get_translator(model_dir, device):
    """Lazily load + cache the translation tokenizer/model for (dir, device)."""
    key = (str(model_dir), device)
    cached = _translator_cache.get(key)
    if cached is not None:
        return cached

    # The translator draws from the same pool as the audio models; free space
    # for it the same budgeted way (a later audio load may re-evict it).
    _make_room(_device_pool(device), device, _incoming_bytes(model_dir))

    from transformers import AutoModelForCausalLM, AutoTokenizer

    log.info(f"Loading prompt translator from {model_dir} on {device}...")
    real_stdout = sys.stdout
    sys.stdout = sys.stderr  # protect the IPC pipe from any library stdout
    hf_env = _force_hf_offline_for_native_load()
    try:
        dtype = torch.float32 if device == "cpu" else torch.bfloat16
        tokenizer = AutoTokenizer.from_pretrained(str(model_dir), local_files_only=True)
        model = AutoModelForCausalLM.from_pretrained(
            str(model_dir), torch_dtype=dtype, local_files_only=True
        )
        model = model.to(device).eval()
    finally:
        _restore_hf_env(hf_env)
        sys.stdout = real_stdout

    _translator_cache[key] = (tokenizer, model)
    log.info(f"Prompt translator ready on {device}.")
    return tokenizer, model


def run_instruct(text, model_dir, device, system_prompt, max_new_tokens=None,
                 repetition_penalty=None, no_repeat_ngram_size=None):
    """Run the instruct LLM (the translator's Qwen) with an ARBITRARY system
    prompt on one short user text. Empty in → empty out. Greedy decoding for
    determinism within a device. This is the general form of ``translate_prompt``:
    the model is a general instruction-follower, so the same loaded weights serve
    translation AND other prompt transforms (variation, abduction, ...) — the only
    difference is the system prompt. Reuses the same lazy/cached loader.

    repetition_penalty / no_repeat_ngram_size default to None and are passed to
    generate() ONLY when a caller sets them (the Re-Prompt/interpret path). Left
    None they are omitted entirely, so translate + the DCO S2 router fall through
    to the model's OWN generation_config — Qwen2.5-1.5B ships
    repetition_penalty=1.1 — making their output BYTE-IDENTICAL to before this
    param existed. (Forcing 1.0 would be WRONG: it overwrites the model's 1.1,
    silently changing every translation AND every S2 routing key, which breaks the
    DCO "same text -> byte-identical recipe" determinism invariant.) Both are
    deterministic logit transforms, so greedy stays deterministic; they exist
    solely so the Re-Prompt path can defeat the degenerate token-cycle a long
    "recombine these freely" palette provokes at the model's default 1.1."""
    # `text` may be a LIST of chat messages instead of one string — the LCO's
    # repair hands the author its own failed answer plus the compiler's
    # complaint, so it edits what it wrote instead of authoring the sound again
    # from nothing. Mirrors run_gguf_instruct.
    if isinstance(text, (list, tuple)):
        turns = [dict(m) for m in text if (m.get("content") or "").strip()]
        if not turns:
            return ""
    else:
        text = (text or "").strip()
        if not text:
            return ""
        turns = [{"role": "user", "content": text}]

    # Resolve the translator device ONCE (may pin to CPU on a small CUDA card)
    # and use it for both the model load and the input tensors — otherwise a
    # CPU-pinned model would meet cuda input_ids and raise a device mismatch.
    tdev = _translator_device(device)
    tokenizer, model = _get_translator(model_dir, tdev)
    messages = [{"role": "system", "content": system_prompt}] + turns
    input_ids = tokenizer.apply_chat_template(
        messages, add_generation_prompt=True, return_tensors="pt"
    ).to(tdev)
    attention_mask = torch.ones_like(input_ids)

    # Build the kwargs so that unset (None) penalties are OMITTED entirely — the
    # call is then byte-for-byte the original generate(), letting the model's own
    # generation_config (Qwen: repetition_penalty=1.1) stand. Only the interpret
    # path sets them, overriding 1.1 with a stronger 1.2 + a hard 3-gram block.
    gen_kwargs = dict(
        do_sample=False,
        num_beams=1,
        pad_token_id=tokenizer.eos_token_id,
    )
    # No arbitrary output cap: a hard cap does not make a reply short, it CUTS it
    # mid-sentence. RepromptStances.cpp carries a whole comment about trimming
    # "function words a mid-sentence max_new_tokens cut tends to leave dangling",
    # i.e. the truncation was observed and worked around downstream instead of
    # removed. Greedy decoding stops at EOS on its own; brevity is the system
    # prompt's job.
    #
    # But OMITTING the parameter is NOT the way to say "no cap": transformers
    # then falls back to GenerationConfig.max_length, whose default is 20 TOKENS
    # (measured, 4.57.6) -- a far harsher guillotine than the caps this replaced.
    # Dropping it cost the Csound author its ADJECTIVES and MOTION lines on every
    # multi-oscillator prompt, and the frozen corpus caught it. So the default is
    # the model's OWN ceiling: everything its context window has left after the
    # prompt. That is a real limit, not an invented one, and it cannot truncate
    # substance -- when it binds at all, generation has already run past the
    # context the model can attend to.
    if max_new_tokens is None:
        cfg = getattr(model, "config", None)
        ctx = int(getattr(cfg, "max_position_embeddings", 0) or 0)
        if not ctx:            # some configs nest it (multimodal wrappers)
            inner = getattr(cfg, "text_config", None)
            ctx = int(getattr(inner, "max_position_embeddings", 0) or 0)
        if not ctx:
            tml = int(getattr(tokenizer, "model_max_length", 0) or 0)
            # the tokenizer sentinel for "no limit" is astronomically large
            ctx = tml if 0 < tml < 10 ** 6 else 0
        used = int(input_ids.shape[-1])
        if not ctx:
            raise RuntimeError(
                "cannot determine the model's context length, so there is no "
                "honest generation ceiling to derive (would silently truncate)")
        # No floor: the remainder IS the ceiling. Flooring it to a "reasonable"
        # number would put back an invented cap, and a floor ABOVE the remainder
        # would promise room the model does not have.
        max_new_tokens = ctx - used - 16
        if max_new_tokens <= 0:
            raise RuntimeError(
                f"prompt of {used} tokens leaves no room in a {ctx}-token "
                f"context window")
    gen_kwargs["max_new_tokens"] = max_new_tokens
    if repetition_penalty is not None:
        gen_kwargs["repetition_penalty"] = repetition_penalty
    if no_repeat_ngram_size is not None:
        gen_kwargs["no_repeat_ngram_size"] = no_repeat_ngram_size

    real_stdout = sys.stdout
    sys.stdout = sys.stderr  # protect the IPC pipe during generate()
    try:
        with torch.no_grad():
            generated = model.generate(input_ids, attention_mask=attention_mask, **gen_kwargs)
    finally:
        sys.stdout = real_stdout

    new_tokens = generated[0, input_ids.shape[1]:]
    return tokenizer.decode(new_tokens, skip_special_tokens=True).strip()


# The external-API author branches (an alternative to the local GGUF for a
# machine that cannot run a 12B model). author_api.py only ever speaks HTTP
# via `requests` (already a hard dependency, see requirements.txt) — no
# heavier SDK, and no dependency on anything the CPU-budget/torch setup at
# the top of this file cares about.
from author_api import call_openai_compatible, call_anthropic  # noqa: E402
import author_api  # noqa: E402  (last_usage() after each external call)


# What the external author has cost since this backend process started.
# Tokens are what the provider bills on, so they are counted for every provider;
# `cost` only accumulates where the provider itself states one (OpenRouter
# reports it in account credits) -- this never multiplies tokens by a price
# table of its own, which would be stale the moment a provider changes its
# rates. Carried over from the maintainer's platform, which threads the same
# token counts through its whole pipeline; its USD column is declared and never
# written, so there was nothing to take on that side.
#
# THREE counters, not one, because they can legitimately disagree and a reader
# who cannot see the disagreement is being misinformed about their own money:
#   calls        -- calls the provider actually SERVED (HTTP 2xx), so a
#                   provider that reports no usage at all (Ollama, an older
#                   self-hosted llama.cpp `server` ignoring stream_options)
#                   still shows up. A call that never got there -- no
#                   connection, a bad key, a 429 -- is not one: nothing was
#                   billed for it, and counting it would both report spending
#                   that did not happen and make every later reading claim
#                   unpriced calls in a session where all of them are priced.
#   token_calls  -- of those, how many came back with a FINAL token count.
#                   Below `calls` means the token figure is a floor, not a
#                   total: either the provider said nothing, or the stream
#                   dropped before the closing count (author_api's `complete`).
#   cost_calls   -- of those, how many came back with a PRICE. Below `calls`
#                   means `cost` covers only part of the session -- printing it
#                   as the total (which an earlier draft did, latching a single
#                   boolean) turns one priced call among a hundred into a
#                   sum-looking number that is wrong by two orders of magnitude.
_API_SPEND = {"calls": 0, "token_calls": 0, "cost_calls": 0,
              "input": 0, "output": 0, "cost": 0.0}


def _account_api_call():
    """Fold the just-finished external call into the session total.

    Called from a `finally`, so it runs for a call that RAISED as well -- which
    is the point: a stream the provider dropped halfway was generated and
    billed, and it is the only failure mode that costs anything."""
    usage = author_api.last_usage()
    if not usage.get("served"):
        return                       # never reached the provider: nothing billed
    _API_SPEND["calls"] += 1
    if "input" not in usage and "output" not in usage:
        return                       # served, but the provider counted nothing
    if usage.get("complete"):
        _API_SPEND["token_calls"] += 1
    _API_SPEND["input"] += int(usage.get("input", 0))
    _API_SPEND["output"] += int(usage.get("output", 0))
    if usage.get("cost") is not None:
        _API_SPEND["cost"] += float(usage["cost"])
        _API_SPEND["cost_calls"] += 1


def api_spend_snapshot():
    return dict(_API_SPEND)


def _resolve_author_backend(request):
    """Resolve which author backend a request wants, local or external API.

    The external-API fields win OUTRIGHT whenever they are fully populated —
    `coder_model_path` is then not consulted at all. The JUCE UI keeps
    local/external as one mutually-exclusive choice, so a request never
    carries both in practice; this precedence is still the documented,
    unambiguous behaviour for a hand-crafted dev/test request. Does not
    restructure _resolve_coder_model_dir's own precedence chain (explicit
    path -> env vars -> installed slot -> scan) — wraps it, does not touch it.

    Returns one of:
      ("api_openai_compat", base_url, api_key, model)
      ("api_anthropic", api_key, model)
      ("local_gguf", gguf_path, model_dir)
      ("local_transformers", model_dir)
      ("unavailable",)
    """
    provider = (request.get("coder_provider") or "").strip()
    api_key = (request.get("coder_api_key") or "").strip()
    api_model = (request.get("coder_api_model") or "").strip()
    if provider and api_model:
        if provider == "openai_compatible":
            base_url = (request.get("coder_api_base") or "").strip()
            # No key requirement here. A key is a PROVIDER's requirement, not
            # this wire's: an OpenAI-compatible endpoint on the user's own
            # machine (Ollama, a local llama.cpp `server`) has no account and
            # no key, and author_api sends the placeholder those shims expect.
            # Demanding one made the keyless case fall through to the LOCAL
            # model while the settings page reported the external author as in
            # use — silently the wrong author, or "not installed" on a machine
            # with no local model, which is exactly who this option is for.
            if base_url:
                return ("api_openai_compat", base_url, api_key, api_model)
        elif provider == "anthropic" and api_key:
            # Anthropic has no keyless form, so here the key IS required, and
            # rejecting it now beats a 401 several seconds into an authoring.
            return ("api_anthropic", api_key, api_model)

    model_dir = _resolve_coder_model_dir(request)
    if model_dir is None:
        return ("unavailable",)
    gguf = _coder_gguf_file(model_dir)
    if gguf is not None:
        return ("local_gguf", gguf, model_dir)
    return ("local_transformers", model_dir)


def _author_backend_label(backend):
    """Human-readable name of the author that actually ran, for the csound
    response's `author_model` provenance field — the UI otherwise shows a
    hardcoded name and cannot tell which model wrote the orchestra (observed
    2026-07-22: the intended slot was unloadable, the resolver walked past it
    to another local model, and nothing on the wire showed it). Same purpose
    now covers the external-API branches too."""
    kind = backend[0]
    if kind == "local_gguf":
        return backend[2].name
    if kind == "local_transformers":
        return backend[1].name
    if kind == "api_openai_compat":
        return f"{backend[3]} (external, {backend[1]})"
    if kind == "api_anthropic":
        return f"{backend[2]} (external, Anthropic)"
    return "unknown"


def run_author_instruct(request, text, system_prompt, device, max_new_tokens=None,
                        **transformers_only):
    """One instruct call on the AUTHOR model — the single LLM in the product.

    Translate, Re-Prompt and the Csound author all run on it. There is no second,
    smaller instruct model: the 1.5B that used to serve translate/Re-Prompt was
    measurably bad at both (it collapsed to a constant answer on the self-check's
    comparison task, which is why that path already asked for the author model
    instead), and keeping a weak model alive for two features while a strong one
    is already resident bought nothing but a download and a second failure mode.

    Local or external API, resolved once via _resolve_author_backend — the
    external branches never see `transformers_only`, mirroring the existing
    GGUF branch below (both are crutch kwargs for the pre-GGUF 1.5B path).

    `transformers_only` carries generate() kwargs that exist only on the
    transformers path (repetition_penalty, no_repeat_ngram_size). They are DROPPED
    for the GGUF author, deliberately: both were crutches for the 1.5B's greedy
    degeneration ("sharp, thin, metallic, buzzing, sharp, thin, ..."), and
    no_repeat_ngram_size is a hard ban rather than a penalty — it spans the prompt
    too, so a system prompt that spells out a required answer form makes that form
    unsayable (measured: "butthe sound measuresbrightandthin"). Re-measured on the
    author model with BOTH off, on the exact palette that provoked the cycle: six
    words, clean stop, zero repeated 3-grams, identical across five runs.
    """
    backend = _resolve_author_backend(request)
    kind = backend[0]
    if kind == "unavailable":
        raise ValueError("Language model is not installed (load it in Settings).")
    if kind == "api_openai_compat":
        _, base_url, api_key, model = backend
        try:
            return call_openai_compatible(base_url, api_key, model, text, system_prompt,
                                          max_new_tokens=max_new_tokens)
        finally:
            _account_api_call()
    if kind == "api_anthropic":
        _, api_key, model = backend
        try:
            return call_anthropic(api_key, model, text, system_prompt,
                                  max_new_tokens=max_new_tokens)
        finally:
            _account_api_call()
    if kind == "local_gguf":
        _, gguf, _model_dir = backend
        return run_gguf_instruct(text, gguf, system_prompt, max_new_tokens=max_new_tokens)
    _, model_dir = backend  # local_transformers
    return run_instruct(text, model_dir, _translator_device(device), system_prompt,
                        max_new_tokens=max_new_tokens, **transformers_only)


def translate_prompt(text, model_dir, device, max_new_tokens=None):
    """Translate one short prompt to English. Empty in → empty out."""
    return run_instruct(text, model_dir, device, TRANSLATION_SYSTEM_PROMPT, max_new_tokens)


# ─── CLAP "ear": machine-listening analysis (the ``analyze`` mode) ─────
#
# CLAP cannot caption (no language decoder); it RANKS audio against a candidate
# text vocabulary in a shared embedding space. The ``analyze`` op returns the
# top-k nearest tags plus signal-level (DSP) spectral descriptors — the learned-
# semantic + measured-physical halves of "what the machine hears". It is the ear
# half of the CLAP→LLM semantic loop ("CLAP is the ear, the LLM is the
# interpreter"); the LLM half is the ``interpret`` op above.
#
# All of this is COPIED from the read-only tooling (tools/clap_probe.py,
# tools/clap_llm_loop.py) on purpose: the backend must stay self-contained for
# PyInstaller — it cannot import from tools/.
#
# CLAP is forced onto the CPU regardless of the generation device: it then never
# allocates Metal-driver memory, so the mps reclaim path can't evict it and it
# can't evict the (mps) audio model — neither starves the other. It still
# participates in the budgeted pool the LRU cache uses via _make_room/_reclaim.

CLAP_MODEL_ID = "laion/clap-htsat-unfused"  # canonical, separable checkpoint (clap_probe)
CLAP_SR = 48_000
# Resident footprint estimate for the budget check (htsat-unfused ≈ 0.6 GB). Used
# as the _make_room `need` so an incoming audio model can evict to fit; honest but
# small, so CLAP does not force-evict real models the way _DEFAULT_UNKNOWN_BYTES
# (8 GB) would.
_CLAP_BYTES = 700 * 1024 ** 2

# The naive timbre strawman (grouped by perceptual axis with deliberate near-
# synonym clusters). Copied verbatim from tools/clap_probe.py NAIVE_VOCAB.
CLAP_NAIVE_VOCAB = [
    # brightness
    "bright", "brilliant", "sparkly", "shimmering", "glittering", "glassy",
    "crystalline", "shiny",
    # darkness
    "dark", "murky", "muddy", "gloomy", "dull", "shadowy", "dim",
    # warmth
    "warm", "cozy", "round", "mellow", "smooth", "velvety", "lush",
    # cold
    "cold", "icy", "sterile", "clinical", "brittle",
    # metallic
    "metallic", "tinny", "clangy", "brassy", "steely", "bell-like", "chrome",
    # harsh
    "harsh", "abrasive", "gritty", "rough", "coarse", "raspy", "scratchy",
    "sharp",
    # soft
    "soft", "gentle", "airy", "breathy", "feathery", "delicate",
    # aggressive
    "aggressive", "punchy", "biting", "cutting", "piercing", "screaming",
    # thick
    "thick", "fat", "heavy", "dense", "massive", "huge", "boomy",
    # thin
    "thin", "weak", "hollow", "reedy", "nasal",
    # organic
    "organic", "woody", "earthy", "natural", "acoustic",
    # digital
    "digital", "synthetic", "artificial", "plasticky", "robotic",
    # noisy
    "noisy", "fuzzy", "distorted", "dirty", "saturated", "crunchy",
    # clean
    "clean", "pure", "pristine", "clear", "transparent",
    # moody
    "dreamy", "ethereal", "haunting", "eerie", "mysterious", "ambient",
    "atmospheric", "cinematic",
    # motion
    "pulsing", "throbbing", "wobbling", "undulating", "evolving", "swirling",
    "morphing",
    # texture extras
    "granular", "glitchy", "watery", "liquid", "gaseous", "percussive",
    "resonant", "droning", "buzzing", "humming",
]

_clap_cache = {}        # {(model_id, device): (model, processor)}
_clap_vocab_cache = {}  # {(model_id, device): (vocab_emb [N,D] cpu tensor, labels)}


@torch.no_grad()
def _clap_embed_texts(model, processor, texts, device, batch=64):
    """Normalized CLAP text embeddings. Copied from tools/clap_probe.py:embed_texts."""
    chunks = []
    for i in range(0, len(texts), batch):
        inputs = processor(text=texts[i: i + batch], return_tensors="pt", padding=True)
        inputs = {k: v.to(device) for k, v in inputs.items()}
        chunks.append(model.get_text_features(**inputs).cpu())
    emb = torch.cat(chunks, 0)
    return torch.nn.functional.normalize(emb, dim=-1)


@torch.no_grad()
def _clap_embed_audios(model, processor, audios, device):
    """Normalized CLAP audio embeddings of a list of 48k mono float32 arrays.
    Copied from tools/clap_probe.py:embed_audios."""
    inputs = processor(audio=audios, sampling_rate=CLAP_SR, return_tensors="pt", padding=True)
    inputs = {k: v.to(device) for k, v in inputs.items()}
    emb = model.get_audio_features(**inputs).cpu()
    return torch.nn.functional.normalize(emb, dim=-1)


@torch.no_grad()
def _clap_assert_sane(model, processor, device):
    """Fail LOUD if CLAP's towers are degenerate / cross-modally misaligned.

    A working CLAP ranks a sine-tone caption higher on sine audio than on noise
    (and vice versa) and does not map all texts to one point. Copied from
    tools/clap_probe.py:assert_model_sane, BUT converted to raise a normal
    RuntimeError (the tool raised SystemExit, which would kill the subprocess) so
    the request loop catches it and returns a standard \\x00 error frame."""
    sr = CLAP_SR
    t = np.arange(sr * 3) / sr
    sine = (0.5 * np.sin(2 * np.pi * 440 * t)).astype("float32")
    rng = np.random.default_rng(0)
    noise = (0.1 * rng.standard_normal(sr * 3)).astype("float32")
    ae = _clap_embed_audios(model, processor, [sine, noise], device)
    te = _clap_embed_texts(
        model, processor,
        ["a steady pure sine wave tone", "harsh white noise static hiss"],
        device,
    )
    sim = te @ ae.T  # rows: [sine-text, noise-text]; cols: [SINE, NOISE]
    tt = (te @ te.T)[0, 1].item()
    sine_ok = (sim[0, 0] - sim[0, 1]).item() > 0.1
    noise_ok = (sim[1, 1] - sim[1, 0]).item() > 0.1
    if not (sine_ok and noise_ok and tt < 0.95):
        raise RuntimeError(
            "CLAP failed the sine-vs-noise sanity check — its text/audio towers "
            "are misaligned or degenerate. "
            f"sine-text·[sine,noise]=[{sim[0,0]:.3f},{sim[0,1]:.3f}] "
            f"noise-text·[sine,noise]=[{sim[1,0]:.3f},{sim[1,1]:.3f}] "
            f"text-text={tt:.3f} (degenerate if ~1.0). "
            "Expected the default laion/clap-htsat-unfused, which passes."
        )
    log.info(
        f"CLAP sanity OK (sine {sim[0,0]:.2f}/{sim[0,1]:.2f}, "
        f"noise {sim[1,0]:.2f}/{sim[1,1]:.2f}, text-text {tt:.2f})"
    )


def _get_clap(device):
    """Lazily load + cache CLAP (ClapModel/ClapProcessor) and its normalized vocab
    text-embeddings for (model_id, device). Mirrors _get_translator: shares the
    budgeted memory pool via _make_room, protects the IPC pipe from library stdout,
    and runs the sanity gate once at load. Unlike the translator, CLAP weights live
    in the HF cache (not a local model dir), so the first call DOWNLOADS them — no
    HF_HUB_OFFLINE guard here."""
    key = (CLAP_MODEL_ID, device)
    cached = _clap_cache.get(key)
    if cached is not None:
        model, processor = cached
        vocab_emb, labels = _clap_vocab_cache[key]
        return model, processor, vocab_emb, labels

    # CLAP draws from the same pool as the audio models; free space the same
    # budgeted way (a later audio load may re-evict residents to fit its own).
    _make_room(_device_pool(device), device, _CLAP_BYTES)

    from transformers import ClapModel, ClapProcessor

    log.info(f"Loading CLAP {CLAP_MODEL_ID} on {device}...")
    real_stdout = sys.stdout
    sys.stdout = sys.stderr  # protect the IPC pipe from any library stdout
    try:
        model = ClapModel.from_pretrained(CLAP_MODEL_ID).to(device).eval()
        processor = ClapProcessor.from_pretrained(CLAP_MODEL_ID)
        _clap_assert_sane(model, processor, device)
        # Pre-compute + cache the normalized vocab text-embeddings ONCE per
        # (model, device) — re-embedding the ~150-tag vocab every request is waste.
        labels = list(dict.fromkeys(CLAP_NAIVE_VOCAB))
        vocab_emb = _clap_embed_texts(model, processor, labels, device)
    finally:
        sys.stdout = real_stdout

    _clap_cache[key] = (model, processor)
    _clap_vocab_cache[key] = (vocab_emb, labels)
    log.info(f"CLAP ready on {device} ({len(labels)} vocab tags).")
    return model, processor, vocab_emb, labels


def spectral_words(audio, sr):
    """Signal-level (DSP) half of the multi-level hearing analysis: BARE physical
    descriptors of the waveform, computed not learned — the complement to CLAP's
    learned-semantic timbre words. DC-removed (>40 Hz). Returns e.g.
    'warm, full-bodied, tonal'. Copied verbatim from tools/clap_llm_loop.py."""
    x = np.asarray(audio, dtype=np.float64)
    if x.ndim > 1:
        x = x.mean(axis=0)
    x = x - x.mean()
    n = x.shape[0]
    if n < 256 or not np.any(x):
        return "silent"
    mag = np.abs(np.fft.rfft(x * np.hanning(n)))
    freqs = np.fft.rfftfreq(n, 1.0 / float(sr))
    keep = freqs >= 40.0
    mag, freqs = mag[keep], freqs[keep]
    total = float(mag.sum())
    if total <= 0.0:
        return "silent"
    centroid = float((freqs * mag).sum() / total)
    eps = 1e-12
    flat = float(np.exp(np.log(mag + eps).mean()) / (mag.mean() + eps))  # tonal↔noisy
    low = float(mag[freqs < 250.0].sum() / total)                        # thin↔bass-heavy
    bright = ("dark" if centroid < 500 else "warm" if centroid < 1500
              else "bright" if centroid < 3500 else "brilliant")
    texture = ("tonal" if flat < 0.10 else "textured" if flat < 0.30 else "noisy")
    body = ("thin" if low < 0.10 else "full-bodied" if low < 0.45 else "bass-heavy")
    return f"{bright}, {body}, {texture}"


def analyze_audio(request):
    """The ``analyze`` op: decode init_audio → CLAP top-k tags + DSP spectral words.

    Returns a JSON string ``{"tags": "...", "spectral": "...", "model": "..."}``
    for a \\x03 text frame. CLAP is forced onto the CPU (see _get_clap). Decode
    reuses the EXACT init_audio wire keys as the generate path
    (_decode_init_audio_request): base64 planar float32 LE in ``init_audio_b64``,
    with ``init_audio_sr`` / ``init_audio_channels``."""
    # Reuse the generate path's tolerant init_audio decode (never raises; a bad
    # payload degrades to None). default_sr is only the fallback when the request
    # omits init_audio_sr.
    init_audio_tuple, _init_noise = _decode_init_audio_request(request, CLAP_SR)
    if init_audio_tuple is None:
        raise ValueError("analyze requires init_audio (init_audio_b64 + init_audio_sr)")
    in_sr, tensor = init_audio_tuple          # tensor: [channels, frames] float32

    audio_cs = tensor.cpu().numpy()           # [channels, frames]
    mono = audio_cs.mean(axis=0).astype("float32")

    topk = max(1, int(request.get("topk", 5)))

    # CLAP forced to CPU regardless of the generation device.
    model, processor, vocab_emb, labels = _get_clap("cpu")

    # Resample to CLAP's 48k for the embedding (spectral_words runs on the
    # native-rate signal — it normalizes by its own sample rate).
    mono_48k = mono
    if in_sr != CLAP_SR:
        import torchaudio
        mono_48k = torchaudio.functional.resample(
            torch.from_numpy(mono), in_sr, CLAP_SR
        ).numpy()

    with torch.no_grad():
        emb = _clap_embed_audios(model, processor, [mono_48k], "cpu")[0]
        sims = emb @ vocab_emb.T               # [N]
        order = torch.argsort(sims, descending=True)[:topk]
    tags = ", ".join(labels[int(i)] for i in order)

    spectral = spectral_words(audio_cs, in_sr)
    return json.dumps({"tags": tags, "spectral": spectral, "model": CLAP_MODEL_ID})


def _patch_scheduler(pipe):
    """Patch scheduler to skip BrownianTree noise at last step (sigma→0 crash)."""
    if not hasattr(pipe.scheduler.step, '__func__'):
        log.info("Scheduler patch not applicable (no __func__)")
        return
    original_step = pipe.scheduler.step.__func__

    def patched_step(self, model_output, timestep, sample, **kwargs):
        step_index = self._step_index
        sigma_next = self.sigmas[step_index + 1] if step_index + 1 < len(self.sigmas) else 0
        if float(sigma_next) == 0.0:
            from diffusers.utils.outputs import BaseOutput
            model_output = self.convert_model_output(model_output, sample=sample)
            self.model_outputs.append(model_output)
            self._step_index += 1

            class Out(BaseOutput):
                prev_sample: torch.Tensor
            return Out(prev_sample=model_output)
        return original_step(self, model_output, timestep, sample, **kwargs)

    pipe.scheduler.step = patched_step.__get__(pipe.scheduler)


def _infer_dtype(device):
    """fp16 on CUDA (halves VRAM, no audible quality loss); fp32 everywhere else.
    MPS has incomplete fp16 coverage; CPU benefits from fp32 precision.

    T5YNTH_CUDA_FP32=1 forces fp32 on CUDA too — an escape hatch for max
    precision / reproducing legacy fp32-cuda presets, and the control arm for
    auditing fp16 audio quality (same seed, both dtypes, same card)."""
    if isinstance(device, str) and device.startswith("cuda"):
        if os.environ.get("T5YNTH_CUDA_FP32") == "1":
            return torch.float32
        return torch.float16
    return torch.float32


def load_pipeline(model_dir, device):
    """Load pipeline on a specific device. Dispatches by format."""
    model_name = model_dir.name
    fmt = _model_formats.get(model_name, "diffusers")
    if fmt == "audioldm2":
        return _load_audioldm2_pipeline(model_dir, device)
    if fmt == "native":
        return _load_native_pipeline(model_dir, device)
    return _load_diffusers_pipeline(model_dir, device)


def _load_diffusers_pipeline(model_dir, device):
    """Load diffusers StableAudioPipeline."""
    from diffusers import StableAudioPipeline

    log.info(f"Loading diffusers pipeline from {model_dir} on {device}...")
    pipe = StableAudioPipeline.from_pretrained(str(model_dir), torch_dtype=_infer_dtype(device))
    pipe = pipe.to(device)

    _patch_scheduler(pipe)

    # Attention slicing reduces peak memory and improves MPS throughput ~20%
    if device in ("mps", "cpu"):
        pipe.enable_attention_slicing()
        log.info(f"Attention slicing enabled for {device}")

    log.info(f"Diffusers pipeline loaded on {device}.")
    return pipe


def _mock_optional_deps():
    """Mock non-essential stable-audio-tools dependencies for minimal import."""
    import types
    import importlib.machinery
    try:
        import pkg_resources  # noqa: F401
    except ImportError:
        import packaging

        pkg_resources = types.ModuleType('pkg_resources')
        pkg_resources.__spec__ = importlib.machinery.ModuleSpec('pkg_resources', None)
        pkg_resources.packaging = packaging
        sys.modules['pkg_resources'] = pkg_resources

    # Do not mock `dac`: the native stable-audio-open-small path imports
    # dac.nn / dac.model during model construction.
    if 'soundfile' not in sys.modules:
        soundfile = types.ModuleType('soundfile')
        soundfile.__spec__ = importlib.machinery.ModuleSpec('soundfile', None)

        def _soundfile_unavailable(*_args, **_kwargs):
            raise RuntimeError("soundfile I/O is not available in the inference subprocess")

        class _UnavailableSoundFile:
            def __init__(self, *_args, **_kwargs):
                _soundfile_unavailable()

        soundfile.read = _soundfile_unavailable
        soundfile.write = _soundfile_unavailable
        soundfile.SoundFile = _UnavailableSoundFile
        sys.modules['soundfile'] = soundfile

    # pytorch_lightning and v_diffusion_pytorch are NOT mocked: the git-main
    # stable-audio-tools build imports `pytorch_lightning as pl` (and uses
    # pl.callbacks) via models/lora/callbacks.py on the DiT import path, and
    # v_diffusion_pytorch is a real runtime dependency. Both are installed in
    # the unified stack (see backend/requirements.txt) — mocking them here would
    # shadow the real packages with stubs and break model construction
    # ("module 'pytorch_lightning' has no attribute 'callbacks'"). The remaining
    # entries are genuine training-only deps that are absent from the inference
    # bundle and never touched on the generate path, so stubbing them keeps the
    # import light without functional loss.
    mocks = ['skimage', 'skimage.transform', 'encodec', 'laion_clap',
             'pedalboard', 'pedalboard.io', 'wandb',
             'gradio', 'jsonmerge', 'clean_fid', 'kornia']
    for name in mocks:
        if name not in sys.modules:
            m = types.ModuleType(name)
            m.__spec__ = importlib.machinery.ModuleSpec(name, None)
            if name == "jsonmerge":
                m.merge = lambda base, override, *args, **kwargs: override if override is not None else base
            sys.modules[name] = m


class NativePipeline:
    """Wrapper for stable-audio-tools native format models."""

    def __init__(self, model, model_config, device, model_name):
        self.model = model
        self.model_config = model_config
        self.device = device
        self.model_name = model_name
        self.sample_size = model_config["sample_size"]
        self.sample_rate = model_config["sample_rate"]
        # Per-model sampler dispatch. SAO uses dpmpp-2m-sde by default; SA3
        # ships with pingpong. The plugin sends an injection_mode but never
        # an explicit sampler_type — the backend decides based on the model's
        # own config so different engines coexist on the same APVTS.
        self.sampler_type = _resolve_sampler_type(model_config, model_name)
        # Number of DiT blocks. layer_split / kombi injection modes need this
        # for slider clamping on the plugin side; it varies per architecture
        # (SAO Small = 16; SA3 Small reads from the loaded model).
        self.dit_blocks = _resolve_dit_block_count(model, model_config)
        # Extract T5 encoder reference for embedding access
        self._t5_conditioner = None
        for key, cond in model.conditioner.conditioners.items():
            if hasattr(cond, 'tokenizer') and hasattr(cond, 'model'):
                self._t5_conditioner = cond
                break

    @property
    def tokenizer(self):
        return self._t5_conditioner.tokenizer if self._t5_conditioner else None

    @property
    def text_encoder(self):
        return self._t5_conditioner.model if self._t5_conditioner else None


def _resolve_sampler_type(model_config, model_name):
    """Pick a sampler_type for native generation, or None to defer to the library.

    Looks at model_config.json's diffusion sub-section first
    (model.diffusion.sampler_type / model.diffusion.sampling.sampler_type),
    then falls back to a per-model-name heuristic. Returns None when the
    config is silent AND the name isn't a known special case — in that case
    ``_generate_native`` omits the kwarg entirely and lets stable-audio-tools
    pick its own default per the model's diffusion_objective (sample_k for
    "v", sample_rf for "rf_denoiser" / "rectified_flow").

    Why None instead of a hard-coded fallback: SAO 1.0 and SAO Small both
    declare diffusion_objective="rf_denoiser" → their config routes through
    sample_rf, whose whitelist is {'euler', 'rk4', 'dpmpp', 'pingpong'}. The
    historical fallback 'dpmpp-2m-sde' belongs to sample_k and is not in
    sample_rf's whitelist; injecting it as a kwarg causes sample_rf to fall
    through its branches and silently return None, surfacing as a
    NoneType.to() AttributeError far downstream.
    """
    diffusion = model_config.get("model", {}).get("diffusion", {}) if isinstance(model_config, dict) else {}
    for path in (("sampler_type",), ("sampling", "sampler_type")):
        node = diffusion
        for key in path:
            node = node.get(key, None) if isinstance(node, dict) else None
            if node is None:
                break
        if isinstance(node, str) and node:
            return node

    # Heuristic fallback by model name — covers cases where the config doesn't
    # record sampler_type explicitly but the model identity is well-known.
    # Stability's SA3 small music checkpoint targets sampler_type="pingpong"
    # per the SA3 model card (8 steps, cfg 1.0). The match is a prefix so a
    # future "stable-audio-3-medium" install lands on pingpong too.
    if model_name.startswith("stable-audio-3"):
        return "pingpong"
    return None


def _dit_block_count_from_config(model_config):
    """Best-effort DiT block count read from model_config.json alone.

    Used by main() during the ready handshake before any model is loaded.
    Falls back to 16 (SAO Small's count) for unknown configs so the plugin
    UI clamps stay sensible on legacy models.
    """
    if not isinstance(model_config, dict):
        return 16
    diffusion = model_config.get("model", {}).get("diffusion", {})
    config = diffusion.get("config", {}) if isinstance(diffusion, dict) else {}
    for key in ("depth", "num_layers", "n_layers", "blocks"):
        value = config.get(key)
        if isinstance(value, int) and value > 0:
            return value
    return 16


def _resolve_dit_block_count(model, model_config):
    """Read DiT block (layer) count for the loaded native model.

    Best source is the inner transformer's layer module list (queried after
    the model object is built). Falls back to the config-only reader for
    architectures whose transformer attribute path differs.
    """
    try:
        layers = model.model.model.transformer.layers
        count = len(layers)
        if count > 0:
            return count
    except AttributeError:
        pass
    return _dit_block_count_from_config(model_config)


def _collect_static_model_metadata(models):
    """Read per-model static metadata without loading the models themselves.

    Returns a dict keyed by model name with at least 'dit_blocks' and
    'sampler_type' for each native model. Models with no readable
    model_config.json (diffusers/audioldm2) are skipped — the plugin treats
    a missing entry as 'use defaults'.
    """
    metadata = {}
    for name, path in models.items():
        fmt = _model_formats.get(name)
        if fmt != "native":
            continue
        cfg_path = Path(path) / "model_config.json"
        if not cfg_path.is_file():
            continue
        try:
            with open(cfg_path) as f:
                cfg = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            log.warning("Could not parse %s: %s", cfg_path, exc)
            continue
        metadata[name] = {
            "dit_blocks": _dit_block_count_from_config(cfg),
            "sampler_type": _resolve_sampler_type(cfg, name),
        }
    return metadata


class AudioLDM2Wrapper:
    """Wrapper for AudioLDM2Pipeline with metadata for routing."""

    def __init__(self, pipe, device):
        self.pipe = pipe
        self.device = device
        self.sample_rate = 16000
        self.target_sample_rate = 44100

    @property
    def tokenizer(self):
        return self.pipe.tokenizer_2

    @property
    def text_encoder(self):
        return self.pipe.text_encoder_2


def _load_audioldm2_pipeline(model_dir, device):
    """Load AudioLDM2Pipeline from diffusers format."""
    from diffusers import AudioLDM2Pipeline

    log.info(f"Loading AudioLDM2 pipeline from {model_dir} on {device}...")
    pipe = AudioLDM2Pipeline.from_pretrained(str(model_dir), torch_dtype=_infer_dtype(device))
    pipe = pipe.to(device)

    if device in ("mps", "cpu"):
        pipe.enable_attention_slicing()
        log.info(f"Attention slicing enabled for AudioLDM2 on {device}")

    log.info(f"AudioLDM2 pipeline loaded on {device}.")
    return AudioLDM2Wrapper(pipe, device)


def _load_native_pipeline(model_dir, device):
    """Load native stable-audio-tools model."""
    _mock_optional_deps()

    # stable-audio-tools imports these via the top-level transformers package.
    # In the frozen bundle, pre-import them explicitly so the lazy export table
    # is populated before the conditioner constructs the T5 stack.
    import transformers
    from transformers.generation.utils import GenerationMixin
    from transformers.models.auto.tokenization_auto import AutoTokenizer
    from transformers.models.t5.modeling_t5 import T5EncoderModel

    # The frozen transformers package can fail to resolve some top-level lazy
    # exports even though the concrete implementation modules are present.
    # stable-audio-tools expects these names on the package root.
    transformers.GenerationMixin = GenerationMixin
    transformers.AutoTokenizer = AutoTokenizer
    transformers.T5EncoderModel = T5EncoderModel

    from stable_audio_tools.models.factory import create_model_from_config
    from stable_audio_tools.models.utils import load_ckpt_state_dict

    config_path = model_dir / "model_config.json"
    weights_path = model_dir / "model.safetensors"
    if not weights_path.is_file():
        raise RuntimeError(
            f"Native model at {model_dir} requires model.safetensors; "
            "checkpoint formats such as model.ckpt are not accepted."
        )

    log.info(f"Loading native pipeline from {model_dir} on {device}...")

    # Suppress any stdout output during model loading (protects IPC pipe)
    real_stdout = sys.stdout
    sys.stdout = sys.stderr
    hf_env = _force_hf_offline_for_native_load()
    try:
        with open(config_path) as f:
            raw_config = json.load(f)

        model_config, resolved_t5_models = _prepare_native_model_config(model_dir, raw_config)

        # The T5 registry patch teaches stable-audio-tools that the bundled
        # t5-base directory satisfies T5Conditioner's assertion on
        # T5_MODELS/T5_MODEL_DIMS. (An earlier second monkey-patch that
        # dispatched gated checkpoints through transformers.AutoModel was
        # removed — it had no remaining caller.)
        # Pingpong is NOT monkey-patched: the pinned stable-audio-tools 0.0.19
        # build implements sampler_type='pingpong' natively (sample_flow_pingpong
        # plus the sample_rf dispatch), and an earlier wrapper that raised
        # NotImplementedError actively prevented SA3 from reaching the real
        # pingpong path. Support is verified at request time in _generate_native
        # via _native_pingpong_supported().
        _patch_stable_audio_tools_t5_registry(resolved_t5_models)
        _patch_apg_for_mps()

        model = create_model_from_config(model_config)
        model.load_state_dict(load_ckpt_state_dict(str(weights_path)))
        model.eval()
        model = model.to(device=device, dtype=_infer_dtype(device))
    finally:
        _restore_hf_env(hf_env)
        sys.stdout = real_stdout

    log.info(f"Native pipeline loaded on {device}.")
    return NativePipeline(model, model_config, device, model_dir.name)


def _native_conditioning_ids(pipe):
    """Return configured native conditioner ids for a stable-audio-tools model."""
    configs = pipe.model_config.get("model", {}).get("conditioning", {}).get("configs", [])
    return {cfg.get("id") for cfg in configs if cfg.get("id")}


def _build_native_conditioning_input(pipe, prompt, duration, start_pos=0.0):
    """Build one metadata dict matching the native model's configured conditioners."""
    payload = {"prompt": prompt}
    cond_ids = _native_conditioning_ids(pipe)

    if "seconds_start" in cond_ids or "seconds_total" in cond_ids:
        start_pos = max(0.0, min(1.0, float(start_pos)))
        virtual_total = duration / (1.0 - start_pos) if start_pos < 1.0 else duration
        if "seconds_start" in cond_ids:
            payload["seconds_start"] = start_pos * virtual_total
        if "seconds_total" in cond_ids:
            payload["seconds_total"] = virtual_total

    return payload


# Deterministic modality routing for SA3 prompts. From v2.5.0 the tonal slot is binary:
# its default is "instrument" = an ISOLATED single instrument, upgraded to "TrackType:
# Music" only when the prompt carries a music signal (tempo / drumtrack / track-form /
# dance-genre / the word "music"). SFX is NEVER produced from a tonal prompt regardless
# of wording — SFX has its own dedicated slot (track_type "sfx" on the SA3 SFX
# checkpoint). The whole scheme is gated by a per-preset "modality epoch" (see
# _native_modality_prefix) so legacy presets keep the old Music/SFX-only behaviour.
# Grounded in the UCDCAE AI Lab preset corpus ("...909 drumtrack, 140bpm" = music;
# "steady saw wave, c3" / "a trombone, c3" / "FUNKY res BASS LINE, c3" = instrument).
#
# Music signals. Matching is on whole lowercased tokens, so "soundtrack" does NOT match
# "track" (listed separately) and "sound" never routes anywhere on its own. NO ensemble
# words (choir/band/group/orchestra): a "heavenly choir" is a valid isolated texture. NO
# bare style adjectives ("funky").
_MUSIC_TEMPO_RE = re.compile(r"\b\d{2,3}\s*bpm\b")
_TOKEN_RE = re.compile(r"[a-z0-9&]+")
_MUSIC_WORDS = {
    # rhythm / drums
    "drumtrack", "drums", "drumbeat", "beat", "beats", "rhythm", "groove",
    "percussion", "909", "808", "breakbeat",
    # explicit musical intent + track / musical form
    "music", "track", "loop", "soundtrack", "jam", "song", "anthem",
    # dance-genre nouns (imply a full arrangement)
    "techno", "house", "trance", "edm", "dubstep", "dnb", "d&b", "jungle",
    "trap", "disco", "samba", "salsa", "reggaeton", "hiphop",
}
_MUSIC_PHRASES = ("drum track", "drum and bass", "hip hop", "hip-hop",
                  "amen break", "backing track")

_SFX_PREFIX = "TrackType: SFX, "
_MUSIC_PREFIX = "TrackType: Music, VocalType: Instrumental, "
_INSTR_PREFIX = "TrackType: Instrument, "

# Modality-epoch sentinel. ABSENCE of "modality_epoch" in a request IS the legacy switch:
# a missing field (old preset, older caller) -> legacy behaviour. The live plugin always
# resolves the per-preset epoch and sends it explicitly (0 for a versionless/legacy
# preset, >=1 for v2.5.0+). Keep epoch>=1 in sync with kModalityEpoch in the C++
# (PluginProcessor.h): 1 == the v2.5.0 behaviour.
_LEGACY_MODALITY_EPOCH = 0


def _looks_like_music(text):
    """Deterministic soft classifier: does this prompt describe MUSIC (rhythmic /
    arranged) rather than an isolated single instrument? Pure string logic, no model
    calls. Drives the Instrument->Music auto-upgrade in _native_modality_prefix."""
    if not text:
        return False
    low = text.lower()
    if _MUSIC_TEMPO_RE.search(low):
        return True
    if any(p in low for p in _MUSIC_PHRASES):
        return True
    return bool(set(_TOKEN_RE.findall(low)) & _MUSIC_WORDS)


def _native_modality_prefix(pipe, track_type=None, prompt_text="",
                            epoch=_LEGACY_MODALITY_EPOCH):
    """Modality prefix prepended to SA3 prompts (Stable Audio 3 paper §5.1).

    The SA3 family is trained with a leading "TrackType: ..." field and the authors
    recommend prepending it at inference — it significantly improves quality. Empty for
    non-SA3 models (SAO, AudioLDM2), trained without it.

    ``epoch`` is the per-preset modality behaviour version (see kModalityEpoch in the
    C++). It makes the routing switchable so legacy presets keep their original prefixes:

      epoch <= 0  (legacy / pre-2.5.0, i.e. any preset saved without the field):
          "sfx" (or a small-sfx name) -> SFX; everything else -> Music. No Instrument,
          no heuristic, no hard selectors — byte-identical to the historical behaviour.

      epoch >= 1  (v2.5.0+): the tonal slot (track_type "instrument") renders
          "TrackType: Instrument" (isolated single instrument) by default, upgraded to
          Music only when ``prompt_text`` carries a music signal (_looks_like_music). A
          tonal prompt is NEVER routed to SFX — SFX comes solely from the dedicated SFX
          slot (track_type "sfx"). Capability still applies: small-sfx renders SFX only,
          small-music and medium render the tonal pair.

    Future heuristic revisions add another `elif epoch == N` branch here.
    """
    name = (getattr(pipe, "model_name", "") or "").lower()
    if not name.startswith("stable-audio-3"):
        return ""

    tt = (track_type or "").strip().lower()

    # ── Legacy (pre-2.5.0): Music / SFX only ──
    if epoch <= 0:
        if tt == "sfx" or (not tt and "sfx" in name):
            return _SFX_PREFIX
        return _MUSIC_PREFIX

    # ── v2.5.0+ : tonal slot is binary Instrument / Music; SFX only via its own slot ──
    # SFX is produced ONLY by the dedicated SFX slot (track_type "sfx") on the SA3 SFX
    # checkpoint — a tonal prompt is NEVER routed to SFX, whatever its wording.
    if tt == "sfx" or (not tt and "sfx" in name):      # the SFX slot
        return _SFX_PREFIX
    if "sfx" in name:                                  # small-sfx can only render SFX
        return _SFX_PREFIX

    # Tonal slot: Instrument by default, upgraded to Music when the prompt signals music
    # (tempo / drumtrack / track-form / dance-genre / the word "music").
    if _looks_like_music(prompt_text):
        return _MUSIC_PREFIX
    return _INSTR_PREFIX


def load_default_model(model_name, devices):
    """Load the default model on the first working device. Returns that device."""
    failures = []
    for dev in devices:
        try:
            get_pipeline(model_name, dev)
            return dev
        except Exception as e:
            msg = f"{dev}: {e}"
            failures.append(msg)
            log.warning(f"Failed to load {model_name} on {msg}")
    failure_summary = "; ".join(failures) if failures else "no devices"
    raise RuntimeError(f"Could not load {model_name} on any device ({failure_summary})")


def startup_model_candidates(models):
    """Return startup candidates in preferred order."""
    preferred = [
        "stable-audio-open-1.0",
        "stable-audio-open-small",
    ]
    ordered = [name for name in preferred if name in models]
    ordered.extend(name for name in models.keys() if name not in ordered)
    return ordered


def choose_startup_model(models, devices):
    """Pick the first model that actually loads on at least one device."""
    failures = []
    for model_name in startup_model_candidates(models):
        try:
            default_device = load_default_model(model_name, devices)
            return model_name, default_device, failures
        except Exception as e:
            msg = f"{model_name}: {e}"
            failures.append(msg)
            log.warning(f"Startup model rejected: {msg}")
    failure_summary = "; ".join(failures) if failures else "no startup candidates"
    raise RuntimeError(f"No usable model could be loaded ({failure_summary})")


# ─── Audio resampling ────────────────────────────────────────────────

def _resample_audio(audio_np, from_sr, to_sr):
    """Resample audio array [channels, samples] from from_sr to to_sr."""
    if from_sr == to_sr:
        return audio_np
    from scipy.signal import resample_poly
    gcd = math.gcd(to_sr, from_sr)
    up, down = to_sr // gcd, from_sr // gcd
    return resample_poly(audio_np, up, down, axis=-1).astype(np.float32)


# ─── Latent cache ────────────────────────────────────────────────────
# Stores pre-VAE latents keyed by name. Enables fast interpolation +
# VAE-only decode (~2s) instead of full diffusion (~15-50s).
# Latents are stored on CPU for device-agnostic access.

_latent_cache = {}   # {name: torch.Tensor on CPU}


def vae_decode(pipe, latent, duration):
    """Decode a latent tensor to audio via VAE. Returns (audio_np, sr)."""
    sr = 44100
    # Cached latents live on CPU (device-agnostic) and may be fp32; the VAE is
    # fp16 on CUDA. Match both device and dtype, else decode raises a mismatch.
    vae_param = next(pipe.vae.parameters())
    latent = latent.to(vae_param.device, vae_param.dtype)
    with torch.no_grad():
        audio = pipe.vae.decode(latent).sample
    audio_np = audio.squeeze(0).cpu().float().numpy()
    requested_samples = int(math.ceil(duration * sr))
    if audio_np.shape[-1] > requested_samples:
        audio_np = audio_np[..., :requested_samples]
    return audio_np, sr


def interpolate_and_decode(pipe, request):
    """Interpolate between cached latents and VAE-decode. ~2s instead of ~15-50s."""
    name_a = request["latent_a"]
    name_b = request["latent_b"]
    alpha = request.get("lerp_alpha", 0.5)
    duration = request.get("duration", 3.0)

    if name_a not in _latent_cache:
        raise ValueError(f"Latent '{name_a}' not in cache (have: {list(_latent_cache.keys())})")
    if name_b not in _latent_cache:
        raise ValueError(f"Latent '{name_b}' not in cache (have: {list(_latent_cache.keys())})")

    lat_a = _latent_cache[name_a]
    lat_b = _latent_cache[name_b]
    interpolated = (1.0 - alpha) * lat_a + alpha * lat_b

    # Optionally cache the interpolated result
    cache_as = request.get("cache_as")
    if cache_as:
        _latent_cache[cache_as] = interpolated.clone()

    t0 = time.time()
    audio_np, sr = vae_decode(pipe, interpolated, duration)
    elapsed = time.time() - t0
    log.info(f"Interpolated {name_a}↔{name_b} (α={alpha:.2f}), decoded in {elapsed:.1f}s")
    return audio_np, sr, -1, elapsed


def decode_cached(pipe, request):
    """Decode a single cached latent. For quick re-listen without diffusion."""
    name = request["latent_name"]
    duration = request.get("duration", 3.0)

    if name not in _latent_cache:
        raise ValueError(f"Latent '{name}' not in cache (have: {list(_latent_cache.keys())})")

    t0 = time.time()
    audio_np, sr = vae_decode(pipe, _latent_cache[name], duration)
    elapsed = time.time() - t0
    log.info(f"Decoded cached '{name}' in {elapsed:.1f}s")
    return audio_np, sr, -1, elapsed


# ─── Semantic axes ───────────────────────────────────────────────────
# Pole prompts matching AxesPanel.cpp kEffectiveAxes (display → key mapping)

SEMANTIC_AXIS_POLES = {
    "music_noise":          ("noise",       "music"),
    "acoustic_electronic":  ("electronic",  "acoustic"),
    "improvised_composed":  ("improvised",  "composed"),
    "refined_raw":          ("raw",         "refined"),
    "solo_ensemble":        ("ensemble",    "solo"),
    "sacred_secular":       ("secular",     "sacred"),
    "tonal_noisy":          ("noisy",       "tonal"),
    "rhythmic_sustained":   ("sustained",   "rhythmic"),
    "rhythmic_sustained_sa3": ("a sustained, continuous, persistent, held, unbroken, droning tone",
                               "a rhythmic, percussive, syncopated, pulsing, staccato, beat-driven groove"),
    "grainy_smooth_sa3":      ("a smooth, clean, pure, polished, even tone",
                               "a grainy, gritty, rough, textured, rasping, abrasive sound"),
    "dense_sparse_sa3":       ("a sparse, single, minimal, bare, isolated sound",
                               "a dense, layered, thick, swarming, saturated texture"),
}

_axis_emb_cache = {}  # {(axis_key, model_name): (dir_tensor, neutral_emb)}


def _apply_semantic_axes(manipulated, axes_dict, encode_fn, model_name, amount=1.0,
                         custom_poles=None):
    """Apply semantic axis deltas to embedding tensor [1, seq, 768].

    axes_dict: {"music_noise": 0.5, "tonal_noisy": -0.3, ...}
    encode_fn: function(text) → (emb[1,seq,768], mask[1,seq])
    amount: master scaler in [0, 1] — attenuates every delta uniformly so
            the user can dial down the off-manifold push that produces
            harsh output without losing the per-axis balance.
    custom_poles: RESEARCH-ONLY (never set by the plugin). {axis_key: (pole_a,
            pole_b)} to inject arbitrary inline pole texts — e.g. synonym-stacked
            "bundled" poles — instead of the shipped SEMANTIC_AXIS_POLES lookup.
            When None the behaviour is byte-identical to the shipped path.

    For each axis, computes direction = pole_emb - neutral_emb,
    then adds direction * value * amount to the manipulated embedding.
    """
    if not axes_dict or amount <= 0.0:
        return manipulated

    # Encode neutral once (cached per model)
    cache_key = ("__neutral__", model_name)
    if cache_key not in _axis_emb_cache:
        neutral_emb, _ = encode_fn("")
        _axis_emb_cache[cache_key] = neutral_emb.detach()
    # Cache is keyed by (text, model_name) only — NOT device/dtype. A session
    # that switches devices (mps fp32 → cuda fp16) would otherwise hand back an
    # fp32 emb for an fp16 manipulated, promoting the sum to fp32 and breaking
    # the fp16 model. Match manipulated's dtype on retrieval. No-op same-dtype.
    neutral_emb = _axis_emb_cache[cache_key].to(manipulated.device, manipulated.dtype)

    for axis_key, value in axes_dict.items():
        if abs(value) < 0.001:
            continue
        poles = (custom_poles or {}).get(axis_key) or SEMANTIC_AXIS_POLES.get(axis_key)
        if not poles:
            continue
        pole_a_text, pole_b_text = poles

        # Cache pole embeddings per model
        for pole_text in (pole_a_text, pole_b_text):
            ck = (axis_key + "_" + pole_text, model_name)
            if ck not in _axis_emb_cache:
                emb, _ = encode_fn(pole_text)
                _axis_emb_cache[ck] = emb.detach()

        emb_a = _axis_emb_cache[(axis_key + "_" + pole_a_text, model_name)].to(manipulated.device, manipulated.dtype)
        emb_b = _axis_emb_cache[(axis_key + "_" + pole_b_text, model_name)].to(manipulated.device, manipulated.dtype)

        # Direction: positive value → pole_b, negative → pole_a
        if value >= 0:
            direction = emb_b - neutral_emb
        else:
            direction = emb_a - neutral_emb

        # Mean-pool direction to [1, 1, dim] for models with variable seq_len
        # (AudioLDM2 uses dynamic T5 padding; SA uses fixed 128-token padding)
        if direction.shape[1] != manipulated.shape[1]:
            direction = direction.mean(dim=1, keepdim=True)

        manipulated = manipulated + direction * (abs(value) * amount)

    return manipulated


# ─── Generation ─────────────────────────────────────────────────────

def _mean_pool(emb, mask):
    """Mean-pool embedding [1, seq, 768] using attention mask → [768]."""
    # mask: [1, seq] → [1, seq, 1]
    m = mask.unsqueeze(-1).float()
    pooled = (emb * m).sum(dim=1) / m.sum(dim=1).clamp(min=1.0)
    return pooled.squeeze(0).cpu().float().numpy()  # [768]


def _echo_through_null(other_emb, null_emb):
    """Spiegelung am Modell-Null: 2·null − other.

    When one prompt field is empty, the empty side's embedding becomes
    the reflection of the present side through the null/unconditional
    encoding (the model's encoding of ""). Standard interpolation
    (1-α)/2·A + (1+α)/2·B then traces a path from the present prompt
    through the model-neutral point to its tonal antipode, instead of
    collapsing to the present prompt for all α.
    """
    return 2.0 * null_emb - other_emb


def _blend_ab(emb_a, emb_b, alpha):
    """A/B crossfade at constant drive: historical direction, pole-length.

    The direction is the historical blend, untouched:
        (0.5 - 0.5a)*A + (0.5 + 0.5a)*B
    Only the LENGTH is restored, to the poles' own norm interpolated across
    alpha and clamped outside [-1, 1] — so |alpha| >= 1 stays byte-identical to
    the extrapolation renorm this replaces (there the reference already was the
    nearer pole's norm, and at exactly +/-1 the ratio is 1.0).

    Why it must also run INSIDE the poles: two prompt embeddings are nearly
    orthogonal (measured cos +0.15..+0.38), so their midpoint is ~0.70 of pole
    length — and length is not decoration here. CFG's unconditional branch is
    literally zero context (dit.py: null_embed = torch.zeros_like(...), and
    to_cond_embed is bias-free), so ||cond|| IS the distance from
    "unconditioned". A short blend is a blend that has drifted back toward the
    model's prior, i.e. toward its most-trained, most-average region. This path
    exists to LEAVE that region, so the middle has to point strangely at FULL
    drive rather than weakly. Length and direction are separable: rescaling
    leaves cos to both poles unchanged to +/-0.002 while drive goes 0.70 -> 0.92
    (SAO) and 0.69 -> 0.83 (SA3). Measured with tools/measure_conditioning_drive.py.
    """
    blend = (0.5 - 0.5 * alpha) * emb_a + (0.5 + 0.5 * alpha) * emb_b
    t = max(-1.0, min(1.0, float(alpha)))
    ref_norm = (0.5 - 0.5 * t) * emb_a.norm() + (0.5 + 0.5 * t) * emb_b.norm()
    res_norm = blend.norm()
    if res_norm > 1e-8:
        blend = blend * (ref_norm / res_norm)
    return blend


def _generate_audioldm2(wrapper, request):
    """Generate audio using AudioLDM2Pipeline with full embedding manipulation.

    AudioLDM2 uses dual embeddings: prompt_embeds (T5 raw, [B,seq,1024]) for
    secondary cross-attention and generated_prompt_embeds (GPT2, [B,8,768]) for
    primary cross-attention. We encode with CFG=True to get properly shaped
    negative+positive pairs, then manipulate the positive halves.
    Output is resampled from 16kHz to 44.1kHz for JUCE compatibility.
    """
    pipe = wrapper.pipe
    prompt_a = request.get("prompt_a", "")
    prompt_b = request.get("prompt_b", "")
    alpha = request.get("alpha", 0.0)
    magnitude = request.get("magnitude", 1.0)
    noise_sigma = request.get("noise_sigma", 0.0)
    duration = request.get("duration", 3.0)
    steps = request.get("steps", 50)
    cfg_scale = request.get("cfg_scale", 3.5)
    seed = request.get("seed", -1)
    dim_offsets = _sanitize_dimension_offsets(request.get("dimension_offsets"))
    injection_mode = request.get("injection_mode", "linear")
    if injection_mode != "linear":
        raise ValueError(
            f"injection_mode='{injection_mode}' is only implemented on the "
            "native pipeline; AudioLDM2 only supports 'linear'."
        )

    if seed < 0:
        import random
        seed = random.randint(0, 2**31 - 1)

    device = wrapper.device
    generator = torch.Generator("cpu").manual_seed(seed)

    with torch.no_grad():
        # Symmetric prompt handling: encode whichever prompts are present.
        # encode_prompt with CFG=True returns [neg, pos] — the negative half
        # is always the encoding of "" and serves as both the CFG anchor and
        # the Spiegel-Punkt for echo derivation.
        a_present = bool(prompt_a)
        b_present = bool(prompt_b)

        if a_present:
            pe_a, mask_a, gpe_a = pipe.encode_prompt(
                prompt=prompt_a, device=device,
                num_waveforms_per_prompt=1,
                do_classifier_free_guidance=True,
                negative_prompt="",
            )
            neg_pe     = pe_a[0:1]
            neg_mask   = mask_a[0:1]
            neg_gpe    = gpe_a[0:1]
            pos_pe_a   = pe_a[1:2]
            pos_mask_a = mask_a[1:2]
            pos_gpe_a  = gpe_a[1:2]

        if b_present:
            pe_b, mask_b, gpe_b = pipe.encode_prompt(
                prompt=prompt_b, device=device,
                num_waveforms_per_prompt=1,
                do_classifier_free_guidance=True,
                negative_prompt="",
            )
            if not a_present:
                neg_pe   = pe_b[0:1]
                neg_mask = mask_b[0:1]
                neg_gpe  = gpe_b[0:1]
            pos_pe_b   = pe_b[1:2]
            pos_mask_b = mask_b[1:2]
            pos_gpe_b  = gpe_b[1:2]

        if not a_present and not b_present:
            pe_e, mask_e, gpe_e = pipe.encode_prompt(
                prompt="", device=device,
                num_waveforms_per_prompt=1,
                do_classifier_free_guidance=True,
                negative_prompt="",
            )
            neg_pe     = pe_e[0:1]
            neg_mask   = mask_e[0:1]
            neg_gpe    = gpe_e[0:1]
            pos_pe_a   = neg_pe.clone()
            pos_pe_b   = neg_pe.clone()
            pos_mask_a = neg_mask
            pos_mask_b = neg_mask
            pos_gpe_a  = neg_gpe.clone()
            pos_gpe_b  = neg_gpe.clone()

        # Echoraum: a leeres Feld is the reflection of the other prompt
        # through neg_pe (the empty-string encoding).
        if a_present and not b_present:
            pos_pe_b   = _echo_through_null(pos_pe_a, neg_pe)
            pos_mask_b = pos_mask_a
            pos_gpe_b  = _echo_through_null(pos_gpe_a, neg_gpe)
        elif b_present and not a_present:
            pos_pe_a   = _echo_through_null(pos_pe_b, neg_pe)
            pos_mask_a = pos_mask_b
            pos_gpe_a  = _echo_through_null(pos_gpe_b, neg_gpe)

        # Pad prompt_embeds to same seq_len (only needed when both prompts
        # were independently encoded — echoes inherit the source's shape).
        if a_present and b_present:
            s_a, s_b = pos_pe_a.shape[1], pos_pe_b.shape[1]
            if s_a != s_b:
                pad_a = max(0, s_b - s_a)
                pad_b = max(0, s_a - s_b)
                if pad_a:
                    pos_pe_a   = torch.nn.functional.pad(pos_pe_a, (0, 0, 0, pad_a))
                    pos_mask_a = torch.nn.functional.pad(pos_mask_a, (0, pad_a))
                    neg_pe     = torch.nn.functional.pad(neg_pe, (0, 0, 0, pad_a))
                    neg_mask   = torch.nn.functional.pad(neg_mask, (0, pad_a))
                if pad_b:
                    pos_pe_b   = torch.nn.functional.pad(pos_pe_b, (0, 0, 0, pad_b))
                    pos_mask_b = torch.nn.functional.pad(pos_mask_b, (0, pad_b))

        # Alpha interpolation: 0 = midpoint, -1 = pure A, +1 = pure B.
        # Constant drive across the whole range (see _blend_ab); each of the two
        # embeddings AudioLDM2 cross-attends to keeps its own pole reference.
        manipulated_pe  = _blend_ab(pos_pe_a,  pos_pe_b,  alpha)
        manipulated_gpe = _blend_ab(pos_gpe_a, pos_gpe_b, alpha)
        pos_mask = (pos_mask_a | pos_mask_b)

        # Magnitude scaling
        if abs(magnitude - 1.0) > 1e-6:
            manipulated_pe = manipulated_pe * magnitude

        # Noise injection (numpy PCG64 for cross-platform determinism)
        if noise_sigma > 0.0:
            rng = np.random.Generator(np.random.PCG64(seed))
            noise_np = rng.standard_normal(manipulated_pe.shape).astype(np.float32)
            noise = torch.from_numpy(noise_np).to(manipulated_pe.device, manipulated_pe.dtype) * (noise_sigma * NOISE_SIGMA_SCALE)
            manipulated_pe = manipulated_pe + noise

        # Semantic axes
        sem_axes = request.get("semantic_axes")
        if sem_axes:
            def audioldm2_encode(text):
                pe, m, _ = pipe.encode_prompt(
                    prompt=text, device=device,
                    num_waveforms_per_prompt=1,
                    do_classifier_free_guidance=False,
                )
                return pe, m
            manipulated_pe = _apply_semantic_axes(
                manipulated_pe, sem_axes, audioldm2_encode, "audioldm2",
                amount=float(request.get("axes_amount", 1.0)),
            )

        # Dimension offsets from DimensionExplorer are always applied last.
        _apply_dimension_offsets(manipulated_pe, dim_offsets)

    offsets_str = f", {len(dim_offsets)} offsets" if dim_offsets else ""
    log.info(f"Generating (AudioLDM2) on {device}: '{prompt_a[:60]}' ({duration}s, {steps} steps, "
             f"CFG={cfg_scale}, mag={magnitude}, noise={noise_sigma}, seed={seed}{offsets_str})")
    t0 = time.time()

    result = pipe(
        prompt_embeds=manipulated_pe,
        attention_mask=pos_mask,
        negative_prompt_embeds=neg_pe,
        negative_attention_mask=neg_mask,
        generated_prompt_embeds=manipulated_gpe,
        negative_generated_prompt_embeds=neg_gpe,
        audio_length_in_s=duration,
        num_inference_steps=steps,
        guidance_scale=cfg_scale,
        generator=generator,
        output_type="np",
    )

    elapsed = time.time() - t0

    audio_np = result.audios[0]
    if audio_np.ndim == 1:
        audio_np = audio_np[np.newaxis, :]

    audio_np = _resample_audio(audio_np, wrapper.sample_rate, wrapper.target_sample_rate)

    if audio_np.shape[0] == 1:
        audio_np = np.vstack([audio_np, audio_np])

    log.info(f"Generated (AudioLDM2) in {elapsed:.1f}s, {audio_np.shape[1]} samples @ 44.1kHz")
    return audio_np, wrapper.target_sample_rate, seed, elapsed, None


def _decode_init_audio_request(request, default_sr):
    """Decode an optional init_audio payload from a generate request (i2i / resynthesis).

    Transport (IPC, MVP): raw float32 little-endian PCM, base64-encoded in the
    JSON field ``init_audio_b64``, planar (channel-major) when multi-channel.
    Companion fields describe it:
      init_audio_sr        int   — sample rate of the supplied audio (Hz)
      init_audio_channels  int   — channel count (default 1)
      init_noise_level     float — denoise strength passed straight through to
                                   stable-audio-tools. 1.0 = full re-generation
                                   (least preservation of the input); lower stays
                                   closer to the input. Default 1.0.

    soundfile I/O is disabled inside the inference subprocess, so audio MUST
    arrive as raw samples like this — never as a file path.

    Returns ``(init_audio_tuple, init_noise_level)`` where ``init_audio_tuple`` is
    either ``None`` (no payload, or an unusable one → caller does plain text-only
    generation) or ``(in_sr, tensor)`` ready for
    ``generate_diffusion_cond[_inpaint]``; that library's ``prepare_audio`` then
    resamples, pad/crops and channel-matches it to the model. Never raises — a
    malformed payload logs a warning and degrades to text-only generation, so a
    bad init_audio can never take down a generation.
    """
    try:
        init_noise_level = float(request.get("init_noise_level", 1.0))
    except (TypeError, ValueError):
        init_noise_level = 1.0  # honour the never-raise contract at the IPC boundary
    b64 = request.get("init_audio_b64")
    if not b64:
        return None, init_noise_level
    try:
        raw = base64.b64decode(b64)
        samples = np.frombuffer(raw, dtype="<f4")
        if samples.size == 0:
            return None, init_noise_level
        channels = max(1, int(request.get("init_audio_channels", 1)))
        in_sr = int(request.get("init_audio_sr", default_sr))
        if in_sr <= 0:
            in_sr = int(default_sr)
        # np.frombuffer returns a read-only view over the decoded bytes; copy to a
        # writable, contiguous float32 array up front so torch.from_numpy below
        # never aliases a non-writable buffer (which it warns about and forbids
        # writing to). astype(copy=True) always copies, even when dtype matches.
        samples = samples.astype(np.float32, copy=True)
        # A single NaN/Inf would flow through the VAE encode and poison the whole
        # diffusion output (and SA3's peak-normalise divides by it → all-NaN
        # audio). Honour the graceful-degradation contract: drop to text-only.
        if not np.all(np.isfinite(samples)):
            log.warning("init_audio contains non-finite samples; "
                        "falling back to text-only generation")
            return None, init_noise_level
        if channels > 1:
            frames = samples.size // channels
            if frames == 0:
                return None, init_noise_level
            samples = samples[: frames * channels].reshape(channels, frames)
        else:
            # prepare_audio runs PadCrop ("n, s = signal.shape") BEFORE it adds a
            # batch dim, so a 1-D [frames] tensor unpacks to 1 value and raises —
            # a crash that lands OUTSIDE this helper's try/except. Always hand the
            # library a 2-D [channels, frames] tensor (here [1, frames]); it then
            # PadCrops, batches and channel-matches it to the model.
            samples = samples.reshape(1, samples.size)
        tensor = torch.from_numpy(samples)
        return (in_sr, tensor), init_noise_level
    except Exception as exc:
        log.warning(
            "init_audio decode failed (%s); falling back to text-only generation",
            exc,
        )
        return None, init_noise_level


def _generate_native(pipe, request):
    """Generate audio using native stable-audio-tools pipeline (e.g. small model).

    Supports the same embedding manipulations as the diffusers path:
    alpha interpolation, magnitude, noise injection, dimension offsets.

    Optional research-mode field "injection_mode" selects how prompt B is mixed
    into prompt A. Default "linear" reproduces the historical crossfade exactly.
    """
    from stable_audio_tools.inference.generation import generate_diffusion_cond

    prompt_a = request.get("prompt_a", "")
    prompt_b = request.get("prompt_b", "")
    # SA3 modality prefix (paper §5.1). Prepend to NON-EMPTY prompts only, so an
    # empty field still encodes as the true "" Spiegel-Punkt (null) reference and
    # the a_present/b_present logic below is unaffected. Empty for non-SA3 models.
    # Classify on the RAW user text (both poles) before the prefix is prepended, so the
    # "instrument" default can auto-upgrade to Music for rhythmic/arranged prompts. The
    # per-preset modality epoch gates the whole scheme; an ABSENT field is the legacy
    # switch (old presets stay Music/SFX) — the live plugin always sends it explicitly.
    _modality_prefix = _native_modality_prefix(
        pipe, request.get("track_type"), (prompt_a + " " + prompt_b),
        int(request.get("modality_epoch", _LEGACY_MODALITY_EPOCH)))
    if _modality_prefix:
        if prompt_a:
            prompt_a = _modality_prefix + prompt_a
        if prompt_b:
            prompt_b = _modality_prefix + prompt_b
    alpha = request.get("alpha", 0.0)
    magnitude = request.get("magnitude", 1.0)
    noise_sigma = request.get("noise_sigma", 0.0)
    duration = request.get("duration", 3.0)
    start_pos = request.get("start_pos", 0.0)
    steps = max(1, int(request.get("steps", 8)))
    cfg_scale = request.get("cfg_scale", 4.0)
    seed = request.get("seed", -1)
    dim_offsets = _sanitize_dimension_offsets(request.get("dimension_offsets"))
    # Dimension Explorer now runs for SA3 too. The edit is confined to the real
    # (non-padded) tokens by _mask_pad below — broadcasting a raw t5gemma-dim push
    # across the 95-99 % learned-padding positions is what previously drove the
    # output off-manifold. Offsets are honoured uniformly for every conditioner
    # here; the per-token confinement + honest masked pooling live further down.
    injection_mode = request.get("injection_mode", "linear")
    injection_transition_at = max(
        0.05, min(0.95, float(request.get("injection_transition_at", 0.6)))
    )
    # late_phase_alpha drives the late-phase blend formula:
    #   late_blend = (0.5 - 0.5α)·A + (0.5 + 0.5α)·B
    # 0  → 50/50 mix  (subtle texture mixing)
    # +1 → pure B      (full A→B at the transition step)
    # The plugin couples this with transition_at so the Fine slider's right
    # end produces effectively pure B (very early transition + pure-B late).
    late_phase_alpha = max(-1.0, min(1.0, float(request.get("late_phase_alpha", 0.0))))
    # Layer mode: two-thumb range slider on the plugin defines the B-zone
    # [split_start, split_end] across the DiT blocks. Per-block weight is a
    # sigmoid product (smooth top-hat) so both edges have ±2-layer ramps.
    # Clamp to the model's actual block count (SAO=16, SA3=20) — a literal 16
    # would pin the top blocks of a deeper DiT to pure-A regardless of slider.
    n_blocks = float(getattr(pipe, "dit_blocks", 16) or 16)
    split_start = max(0.0, min(n_blocks, float(request.get("split_start", 4.0))))
    split_end   = max(0.0, min(n_blocks, float(request.get("split_end",  n_blocks))))
    layer_split_smoothness = 2.0  # sigmoid scale; ±2 layers ramp at each edge
    if injection_mode not in ("linear", "delta", "late_step", "layer_split",
                              "kombi1", "kombi2", "kombi3"):
        raise ValueError(
            f"Unknown injection_mode '{injection_mode}'. "
            "Expected one of: linear, delta, late_step, layer_split, kombi1, kombi2, kombi3."
        )

    # Kombi 1/2/3: late-step blend × layer-split. The bands are FRACTIONS of the
    # model's DiT depth so the geometry stays proportional across models (SAO=16,
    # SA3=20): kombi1 = lower quarter, kombi2 = broad mid (quarter→three-quarter),
    # kombi3 = narrow centre. On a 16-block DiT these reduce to the historical
    # 0-4 / 4-12 / 6-10. The frontend computes the identical fractions of
    # ditBlocks_ and sends them; the backend re-asserts here as a safety net so
    # stale presets or drift overrides can't desync the geometry. Both sides MUST
    # move together (see PromptPanel::buildInferenceRequest).
    if injection_mode == "kombi1":
        split_start, split_end = 0.0, 0.25 * n_blocks            # surface (low layers)
    elif injection_mode == "kombi2":
        split_start, split_end = 0.25 * n_blocks, 0.75 * n_blocks  # broad mid
    elif injection_mode == "kombi3":
        split_start, split_end = 0.375 * n_blocks, 0.625 * n_blocks  # narrow centre

    if seed < 0:
        import random
        seed = random.randint(0, 2**31 - 1)

    sr = pipe.sample_rate
    device = pipe.device
    effective_steps = steps
    if getattr(pipe.model, "diffusion_objective", None) == "v" and effective_steps < 2:
        log.warning(
            "Native %s requested with %d step; clamping to 2 because the k-diffusion sampler "
            "is unstable below 2 steps",
            pipe.model_name,
            effective_steps,
        )
        effective_steps = 2

    # ── Encode prompts via the model's built-in T5 conditioner ──
    # Then manipulate embeddings (alpha, magnitude, noise, dim offsets)
    # and pass as pre-computed conditioning_tensors.
    with torch.no_grad():
        a_present = bool(prompt_a)
        b_present = bool(prompt_b)

        # Always encode "" for the Spiegel-Punkt and as conditioning carrier
        # (for timing fields like seconds_start/seconds_total when neither
        # prompt is present).
        cond_null = pipe.model.conditioner(
            [_build_native_conditioning_input(pipe, "", duration, start_pos)],
            device,
        )
        null_pe = cond_null["prompt"][0]
        null_mask = cond_null["prompt"][1]

        if a_present:
            cond_a = pipe.model.conditioner(
                [_build_native_conditioning_input(pipe, prompt_a, duration, start_pos)],
                device,
            )
            prompt_emb_a = cond_a["prompt"][0]   # [1, seq, 768]
            mask_a = cond_a["prompt"][1]
        else:
            # Carry timing/duration conditioning from cond_null. Deep-copy so
            # the later cond_a["prompt"] = [...] assignment doesn't corrupt
            # cond_null in case anything else holds a reference.
            cond_a = copy.deepcopy(cond_null)
            prompt_emb_a = null_pe.clone()
            mask_a = null_mask if null_mask is None else null_mask.clone()

        if b_present:
            cond_b = pipe.model.conditioner(
                [_build_native_conditioning_input(pipe, prompt_b, duration, start_pos)],
                device,
            )
            prompt_emb_b = cond_b["prompt"][0]
            mask_b = cond_b["prompt"][1]
        else:
            prompt_emb_b = null_pe.clone()
            mask_b = null_mask

        # Echoraum: a leeres Feld is the reflection of the other prompt
        # through null_pe (encoding of "").
        if a_present and not b_present:
            prompt_emb_b = _echo_through_null(prompt_emb_a, null_pe)
            mask_b = mask_a
        elif b_present and not a_present:
            prompt_emb_a = _echo_through_null(prompt_emb_b, null_pe)
            mask_a = mask_b

        # Masks first — both the explorer pooling and the padding handling
        # below depend on them.
        combined_mask = None
        if mask_a is not None and mask_b is not None:
            combined_mask = (mask_a | mask_b)
        elif mask_a is not None:
            combined_mask = mask_a
        else:
            combined_mask = mask_b

        # Auto-detect a LEARNED (non-zero) padding embedding. SA3's t5gemma
        # conditioner emits a non-zero embedding at padded positions that the
        # DiT cross-attends to (it disables the cond mask — paper §2.2); zeroing
        # it feeds out-of-distribution conditioning and collapses the latent to
        # a ~10.76 Hz buzz. SAO's T5 conditioner emits true zeros there, so
        # masking is a safe no-op that also scrubs manipulation pollution.
        # Inspect the raw (pre-manipulation) embedding at masked positions.
        _learned_padding = False
        if combined_mask is not None:
            _pad_pos = ~combined_mask.bool()  # True at padded positions
            if bool(_pad_pos.any()):
                _learned_padding = prompt_emb_a[_pad_pos].abs().mean().item() > 1e-4

        def _mask_pad(emb):
            """Confine every embedding manipulation to the REAL (non-padded)
            tokens. Two conditioner regimes:
              • TRUE-ZERO padding (SAO's T5): zero the padded positions, which
                both restores padding to zero AND scrubs manipulation pollution.
              • LEARNED padding (SA3's t5gemma): the DiT cross-attends to a
                non-zero sentinel at padded positions, so zeroing it collapses
                the latent to a ~10.76 Hz buzz. Instead RESTORE the raw sentinel
                (from the un-manipulated prompt_emb_a) — this leaves the learned
                padding untouched while keeping magnitude / noise / axes / dim
                offsets on the real tokens only, where the semantic content
                lives. Broadcasting an edit across the 95-99 % padded positions
                is what pushed SA3 manipulation off-manifold.
            No-op when there is no mask, or when nothing was manipulated (the
            un-manipulated blend already carries the raw sentinel at padding)."""
            # RESEARCH-ONLY (never set by the plugin; shipped behaviour unchanged):
            # apply every manipulation to the FULL conditioning incl. learned
            # padding, reproducing the Fedora Latent Lab's unmasked axis push
            # (result_emb += t·pca[k] over all 256 positions) for A/B experiments.
            if request.get("unmask_manipulation", False):
                return emb
            if combined_mask is None:
                return emb
            if _learned_padding:
                out = emb.clone()
                pad = ~combined_mask.bool()  # [1, seq]
                out[pad] = prompt_emb_a[pad].to(out.dtype)
                return out
            return emb * combined_mask.unsqueeze(-1).float()

        def _pooled(emb, mask):
            """Pool an embedding [1, seq, 768] → [768] for the GUI explorer.
            Masked mean over the REAL tokens when the conditioner uses learned
            padding (SA3) — a naive full-sequence mean dilutes the true per-token
            divergence ~20-80× across the padded positions. SAO's true-zero
            padding keeps the historical naive mean (byte-identical bars)."""
            if _learned_padding and mask is not None:
                return _mean_pool(emb, mask)
            return emb.squeeze(0).mean(dim=0).cpu().float().numpy()

        # Mean-pool for DimensionExplorer stats (post-echo, pre-manipulation)
        emb_a_pooled = _pooled(prompt_emb_a, mask_a)
        emb_b_pooled = _pooled(prompt_emb_b, mask_b)

        # ── Embedding-level manipulators ──
        # Magnitude / Noise / Semantic Axes / Dimension Offsets all act
        # additively or multiplicatively on a 768-D embedding tensor. To make
        # them work uniformly across all injection modes (Linear feeds a
        # single blend tensor into the DiT; the non-linear modes feed two
        # tensors — early/late or A/B per block — that the per-block hooks
        # combine), we apply the SAME shifts to every tensor that flows
        # into the model. Because the manipulations are linear, applying
        # them independently to A and B and then blending is equivalent to
        # blending first and then manipulating (modulo the renorm step that
        # only fires when |alpha| > 1, which is Linear-specific).
        sem_axes = request.get("semantic_axes")
        axes_amount = float(request.get("axes_amount", 1.0))

        # Semantic axes now run for SA3 too, with one KNOWN LIMITATION: the axis
        # direction = encode(pole) − encode("") still uses t5gemma's learned
        # padding as the neutral "zero line", so the poles come out somewhat
        # unbalanced / under-scaled vs SAO's T5 (a proper per-SA3 recompute of the
        # pole texts is the real fix). The push is at least confined to the real
        # tokens by _mask_pad below, so it is no longer swamped by the padding.
        # SAO and AudioLDM2 keep their validated axes untouched.

        def native_encode(text):
            cond = pipe.model.conditioner(
                [_build_native_conditioning_input(pipe, text, 1.0, 0.0)],
                device,
            )
            return cond["prompt"][0], None

        # Noise tensor is drawn ONCE from the seed-derived PCG64 RNG and
        # reused across every embedding path so the per-mode geometry
        # (e.g. blends, layer mixes) keeps the Linear-mode invariant of
        # "noise added once, not per term".
        _noise_tensor = None
        if noise_sigma > 0.0:
            rng = np.random.Generator(np.random.PCG64(seed))
            noise_np = rng.standard_normal(prompt_emb_a.shape).astype(np.float32)
            # Match the embedding dtype (fp16 on CUDA) so the later add stays in
            # the model's dtype — promotion to fp32 here would feed an fp32 cond
            # into the fp16 DiT and raise a dtype-mismatch. No-op on fp32 paths.
            _noise_tensor = torch.from_numpy(noise_np).to(prompt_emb_a.device, prompt_emb_a.dtype) * (noise_sigma * NOISE_SIGMA_SCALE)

        def apply_pre_offsets(emb):
            """Magnitude + noise + semantic axes — i.e. all manipulations
            EXCEPT DimensionExplorer offsets. Returns a new tensor; the
            input is not mutated. Mask is NOT applied here."""
            out = emb
            if abs(magnitude - 1.0) > 1e-6:
                out = out * magnitude
            if _noise_tensor is not None:
                out = out + _noise_tensor
            if sem_axes:
                out = _apply_semantic_axes(out, sem_axes, native_encode, pipe.model_name,
                                           amount=axes_amount,
                                           custom_poles=request.get("semantic_axes_poles"))
            return out

        def apply_dim_offsets(emb):
            """DimensionExplorer offsets, applied in-place on a clone so the
            caller's tensor is preserved (multiple paths may share storage
            with the raw prompt embedding)."""
            if not dim_offsets:
                return emb
            out = emb.clone()
            _apply_dimension_offsets(out, dim_offsets)
            return out

        def apply_all(emb):
            return apply_dim_offsets(apply_pre_offsets(emb))

        # ── Injection mode dispatch ──
        # "linear"     : historical crossfade (default; byte-identical to v1.5).
        # "delta"      : A + alpha · (B − null), preserves A more strongly.
        # "late_step"  : early sampler steps see A only; after the transition
        #                point the DiTWrapper.forward swap injects late_blend.
        # "layer_split": every DiT block sees a different A/B blend, sigmoid-
        #                ramped over the [split_start, split_end] B-zone.
        # "kombi1/2/3" : step phase × layer band, hard-mask layer geometry.
        # Manipulations are applied to whichever tensor(s) actually flow
        # into the cross-attention layer for the active mode.
        late_step_payload = None
        layer_split_payload = None  # (proj_a, proj_b, proj_null, blocks)
        kombi_payload = None        # (proj_a, proj_late, proj_null, blocks)
        baseline_pooled = None      # set per mode for the GUI explorer feedback
        if injection_mode == "linear":
            manipulated = _blend_ab(prompt_emb_a, prompt_emb_b, alpha)
            # Inline manipulation chain so baseline_pooled snapshots the
            # post-axes-pre-DimExplorer state (the GUI explorer's "this is what
            # the model sees before your dim offsets" reference contract).
            # NOT byte-identical to v1.5.x inside |alpha| < 1 any more: the
            # blend now holds pole drive there (see _blend_ab). |alpha| >= 1
            # is unchanged.
            manipulated = apply_pre_offsets(manipulated)
            baseline = _mask_pad(manipulated)
            baseline_pooled = _pooled(baseline, combined_mask)
            manipulated = apply_dim_offsets(manipulated)
        elif injection_mode == "delta":
            manipulated = prompt_emb_a + alpha * (prompt_emb_b - null_pe)
            manipulated = apply_pre_offsets(manipulated)
            baseline = _mask_pad(manipulated)
            baseline_pooled = _pooled(baseline, combined_mask)
            manipulated = apply_dim_offsets(manipulated)
        elif injection_mode == "late_step":
            # Late-phase blend driven by late_phase_alpha (set by the Fine slider).
            # α=0 → 50/50 mix (subtle); α=+1 → pure B (slider's right end).
            # Both the early A-only path AND the late blend get the same
            # manipulations so semantic axes / noise / magnitude / dim offsets
            # shape both phases consistently.
            la = late_phase_alpha
            late_blend = _blend_ab(prompt_emb_a, prompt_emb_b, la)
            manipulated = apply_all(prompt_emb_a)         # early-phase conditioning
            late_emb_manip = apply_all(late_blend)        # late-phase conditioning
            late_emb_zeroed = _mask_pad(late_emb_manip)
            late_step_payload = (late_emb_zeroed, combined_mask)
            baseline_pooled = _pooled(manipulated, combined_mask)
        elif injection_mode == "layer_split":
            # Per-block context override via forward_pre_hook on each
            # block.cross_attn. Pre-project A, B, null through to_cond_embed
            # so the hook bypasses the DiT's projection step cleanly.
            # Manipulate A and B independently (with the same shifts) BEFORE
            # projection so the per-block sigmoid blend (1−w)·A + w·B inherits
            # magnitude / noise / axes / dim offsets uniformly.
            emb_a_manip = apply_all(prompt_emb_a)
            emb_b_manip = apply_all(prompt_emb_b)
            emb_a_masked = _mask_pad(emb_a_manip)
            emb_b_masked = _mask_pad(emb_b_manip)
            dit = pipe.model.model.model  # DiffusionTransformer
            param_dtype = next(dit.to_cond_embed.parameters()).dtype
            with torch.no_grad():
                proj_a    = dit.to_cond_embed(emb_a_masked.to(param_dtype))
                proj_b    = dit.to_cond_embed(emb_b_masked.to(param_dtype))
                # CFG-uncond half: the model uses `torch.zeros_like(cross_attn_cond)`
                # AFTER `to_cond_embed` (dit.py:141 projects, then dit.py:346 builds
                # null_embed at the projected level). Match that exactly — projecting
                # an encoded "" string would NOT match what the unhooked path does.
                proj_null = torch.zeros_like(proj_a)
            blocks = dit.transformer.layers
            layer_split_payload = (proj_a, proj_b, proj_null, blocks)
            manipulated = emb_a_manip  # un-hooked fallback / global cond path
            baseline_pooled = _pooled(manipulated, combined_mask)
        else:  # kombi1 / kombi2 / kombi3 — late-step × layer-split.
            # Kombi semantics: early sampler steps see pure A on every block.
            # After the transition step, each block in the per-Kombi
            # hardcoded layer band sees the late_blend; blocks outside the
            # band still see A (hard mask). Same manipulations applied to
            # both the A path and the late_blend path before projection.
            la = late_phase_alpha
            late_blend = _blend_ab(prompt_emb_a, prompt_emb_b, la)
            emb_a_manip = apply_all(prompt_emb_a)
            late_blend_manip = apply_all(late_blend)
            emb_a_masked = _mask_pad(emb_a_manip)
            late_blend_masked = _mask_pad(late_blend_manip)
            dit = pipe.model.model.model
            param_dtype = next(dit.to_cond_embed.parameters()).dtype
            with torch.no_grad():
                proj_a    = dit.to_cond_embed(emb_a_masked.to(param_dtype))
                proj_late = dit.to_cond_embed(late_blend_masked.to(param_dtype))
                proj_null = torch.zeros_like(proj_a)
            blocks = dit.transformer.layers
            kombi_payload = (proj_a, proj_late, proj_null, blocks)
            manipulated = emb_a_manip  # un-hooked fallback / global cond path
            baseline_pooled = _pooled(manipulated, combined_mask)

        # Scrub manipulation pollution from padded positions. For conditioners
        # with TRUE-ZERO padding (SAO's T5) this restores padding to zero; for
        # LEARNED padding (SA3's t5gemma, which the DiT cross-attends to since it
        # disables the cond mask — paper §2.2) _mask_pad is a no-op so the learned
        # embedding survives. Auto-detected above via _learned_padding.
        manipulated = _mask_pad(manipulated)

        # Replace prompt embedding in conditioning tensors
        cond_a["prompt"] = [manipulated, combined_mask]

    offsets_str = f", {len(dim_offsets)} offsets" if dim_offsets else ""
    mode_str = f", mode={injection_mode}" if injection_mode != "linear" else ""
    log.info(f"Generating (native) on {device}: '{prompt_a[:60]}' ({duration}s, {effective_steps} steps, "
             f"CFG={cfg_scale}, mag={magnitude}, noise={noise_sigma}, seed={seed}{offsets_str}{mode_str})")
    t0 = time.time()

    # late_step: install a forward swap on DiTWrapper so the cross-attn
    # conditioning flips from "early" (= manipulated stored in cond_a) to
    # "late" (= late_step_payload[0]) after a fraction of the sampler steps.
    # ConditionedDiffusionModelWrapper.forward (diffusion.py:216) passes
    # cross_attn_cond / cross_attn_mask as kwargs to DiTWrapper, so kwargs
    # swap suffices. DiTWrapper.forward asserts batch_cfg=True, so the hook
    # fires exactly once per sampler step.
    restore_forward = None
    if injection_mode == "late_step" and late_step_payload is not None:
        late_emb_zeroed, late_mask = late_step_payload
        transition_step = max(1, int(round(effective_steps * injection_transition_at)))
        dit_wrapper = pipe.model.model
        original_forward = dit_wrapper.forward
        state = {"calls": 0}

        def patched_forward(x, t, **kwargs):
            if state["calls"] >= transition_step:
                kwargs["cross_attn_cond"] = late_emb_zeroed
                # Use DiTWrapper's parameter name `cross_attn_mask` (NOT the
                # inner DiffusionTransformer's `cross_attn_cond_mask`).
                # DiTWrapper.forward takes `cross_attn_mask=...` and internally
                # forwards it as `cross_attn_cond_mask` to the inner DiT
                # (diffusion.py:547). Setting `cross_attn_cond_mask` here would
                # collide with that forwarding and TypeError-out.
                kwargs["cross_attn_mask"] = late_mask
            state["calls"] += 1
            return original_forward(x, t, **kwargs)

        dit_wrapper.forward = patched_forward

        def restore_forward():
            if dit_wrapper.forward is patched_forward:
                del dit_wrapper.forward

    elif injection_mode == "layer_split" and layer_split_payload is not None:
        # Register a forward_pre_hook on every block.cross_attn that overrides
        # the `context` kwarg with a per-block A↔B blend. Per-layer weight is
        # the product of two sigmoids → smooth top-hat over [split_start, split_end].
        proj_a, proj_b, proj_null, blocks = layer_split_payload
        num_layers = len(blocks)
        # Special cases for clean limits at the slider extremes:
        # • Both thumbs at the outer edges → full B (avoids the 0.5-edge artifact
        #   of the sigmoid product).
        # • Empty range (start ≥ end) → no B at all (pure A pass-through).
        full_b = (split_start <= 0.5 and split_end >= num_layers - 0.5)
        empty  = (split_start >= split_end)
        per_block_ctx = []  # one tensor per block; CFG-doubling at hook fire time
        for i in range(num_layers):
            if full_b:
                w = 1.0
            elif empty:
                w = 0.0
            else:
                w_left  = 1.0 / (1.0 + math.exp(-(i - split_start) / layer_split_smoothness))
                w_right = 1.0 / (1.0 + math.exp(-(split_end - i)   / layer_split_smoothness))
                w = w_left * w_right
            blended = (1.0 - w) * proj_a + w * proj_b
            per_block_ctx.append(blended)

        hook_handles = []
        for i, block in enumerate(blocks):
            ctx_cond = per_block_ctx[i]            # [B, seq, embed]
            ctx_uncond = proj_null                  # [B_null, seq, embed]

            def make_hook(ctx_cond_local, ctx_uncond_local):
                def pre_hook(module, args, kwargs):
                    # The DiT calls block.cross_attn(x, context=context, ...).
                    # When CFG is active the runtime context has batch=2*B
                    # (cond half + uncond half). We reconstruct the same
                    # batching from our pre-projected per-block tensors,
                    # repeating to match the runtime context's batch dim if
                    # needed (covers cfg=1 and cfg>1 uniformly).
                    runtime_ctx = kwargs.get("context")
                    if runtime_ctx is None and len(args) >= 2:
                        runtime_ctx = args[1]
                    if runtime_ctx is None:
                        return None  # nothing to override
                    b_runtime = runtime_ctx.shape[0]
                    b_cond = ctx_cond_local.shape[0]
                    if b_runtime == b_cond:
                        new_ctx = ctx_cond_local
                    elif b_runtime == 2 * b_cond:
                        new_ctx = torch.cat([ctx_cond_local, ctx_uncond_local], dim=0)
                    else:
                        # Unexpected batching — let the original through.
                        return None
                    new_ctx = new_ctx.to(runtime_ctx.dtype).to(runtime_ctx.device)
                    new_kwargs = dict(kwargs)
                    new_kwargs["context"] = new_ctx
                    return args, new_kwargs
                return pre_hook

            handle = block.cross_attn.register_forward_pre_hook(
                make_hook(ctx_cond, ctx_uncond), with_kwargs=True
            )
            hook_handles.append(handle)

        def restore_forward():
            for h in hook_handles:
                h.remove()

    elif injection_mode in ("kombi1", "kombi2", "kombi3") and kombi_payload is not None:
        # Kombi: late-step × layer-split. Per-block hooks override cross_attn
        # context every step, but switch between an "early" and "late" tensor
        # based on a sampler-step counter ticked by a DiTWrapper.forward patch.
        #   early: pure A on every block (uniform across layers).
        #   late : per-layer (1-w)·A + w·late_blend, sigmoid top-hat over the
        #          per-Kombi hardcoded [split_start, split_end] range.
        proj_a, proj_late, proj_null, blocks = kombi_payload
        num_layers = len(blocks)
        # Hard layer mask (no sigmoid softening): w = 1 inside [start, end),
        # 0 outside. Different from layer_split's sigmoid top-hat, which
        # softens edges to ~0.5 at the boundary. The Kombis are preset
        # geometry — the user's intensity slider controls the late-phase
        # blend, NOT the band shape — so a hard mask makes "slider=1" mean
        # "100% late_blend on every block in the band", matching the user's
        # mental model of a preset's full strength.
        late_per_block = []
        for i in range(num_layers):
            w = 1.0 if (i >= split_start and i < split_end) else 0.0
            late_per_block.append((1.0 - w) * proj_a + w * proj_late)

        transition_step = max(1, int(round(effective_steps * injection_transition_at)))
        state = {"calls": 0}

        # Patch DiTWrapper.forward to tick the step counter once per sampler
        # step (it's the unique entry-point per step; the inner DiT calls all
        # 16 blocks within a single forward). Increment AFTER original_forward
        # so all hooks within one step see the same `calls` value.
        dit_wrapper = pipe.model.model
        original_forward = dit_wrapper.forward

        def patched_forward(x, t, **kwargs):
            result = original_forward(x, t, **kwargs)
            state["calls"] += 1
            return result

        dit_wrapper.forward = patched_forward

        hook_handles = []
        for i, block in enumerate(blocks):
            ctx_early_local  = proj_a
            ctx_late_local   = late_per_block[i]
            ctx_uncond_local = proj_null

            def make_hook(early, late, uncond):
                def pre_hook(module, args, kwargs):
                    runtime_ctx = kwargs.get("context")
                    if runtime_ctx is None and len(args) >= 2:
                        runtime_ctx = args[1]
                    if runtime_ctx is None:
                        return None
                    ctx_cond = late if state["calls"] >= transition_step else early
                    b_runtime = runtime_ctx.shape[0]
                    b_cond = ctx_cond.shape[0]
                    if b_runtime == b_cond:
                        new_ctx = ctx_cond
                    elif b_runtime == 2 * b_cond:
                        new_ctx = torch.cat([ctx_cond, uncond], dim=0)
                    else:
                        return None
                    new_ctx = new_ctx.to(runtime_ctx.dtype).to(runtime_ctx.device)
                    new_kwargs = dict(kwargs)
                    new_kwargs["context"] = new_ctx
                    return args, new_kwargs
                return pre_hook

            handle = block.cross_attn.register_forward_pre_hook(
                make_hook(ctx_early_local, ctx_late_local, ctx_uncond_local),
                with_kwargs=True,
            )
            hook_handles.append(handle)

        def restore_forward():
            for h in hook_handles:
                h.remove()
            if dit_wrapper.forward is patched_forward:
                del dit_wrapper.forward

    # stable_audio_tools prints seed + tqdm progress to stdout, which would
    # corrupt the binary IPC pipe to JUCE. Redirect stdout → stderr temporarily.
    real_stdout = sys.stdout
    sys.stdout = sys.stderr
    # sampler_type is per-model and only passed when the model requests one
    # explicitly (currently only SA3 → "pingpong"). For models where
    # _resolve_sampler_type returned None (SAO 1.0, SAO Small, anything else
    # without a config sampler_type field), we omit the kwarg so
    # stable-audio-tools uses its own default per the model's
    # diffusion_objective: sample_k's default for v-objective models, or
    # sample_rf's default ('euler') for rf_denoiser / rectified_flow models.
    # Forcing a value here is dangerous because sample_rf's whitelist is a
    # short {'euler','rk4','dpmpp','pingpong'}; anything else makes it
    # silently return None and surfaces as NoneType.to() far downstream.
    generate_kwargs = {}
    # i2i / resynthesis (optional): init_audio drives generation from an existing
    # waveform — stable-audio-tools encodes it into the VAE latent and denoises
    # from there instead of from pure noise, while the embedding manipulation
    # above still conditions the text side (two orthogonal channels). Both native
    # entry points take the same init_audio=(sr, tensor) + init_noise_level kwargs
    # (SAO generate_diffusion_cond; SA3 generate_diffusion_cond_inpaint, where an
    # init_audio with no inpaint_mask is a plain all-zero-mask variation). When no
    # payload is present nothing is added, so the historical text-only path is
    # byte-for-byte unchanged.
    init_audio_tuple, init_noise_level = _decode_init_audio_request(request, sr)
    if init_audio_tuple is not None:
        _ia_sr, _ia_tensor = init_audio_tuple
        # init_audio is VAE-encoded by the model's pretransform, which carries
        # the model dtype (fp16 on CUDA). The decode hands back fp32, so cast it
        # to match — otherwise the encode raises an fp16/fp32 mismatch. The
        # library's prepare_audio handles device placement but not dtype. No-op
        # on fp32 paths (mps/cpu).
        _ia_tensor = _ia_tensor.to(next(pipe.model.parameters()).dtype)
        init_audio_tuple = (_ia_sr, _ia_tensor)
        generate_kwargs["init_audio"] = init_audio_tuple
        generate_kwargs["init_noise_level"] = init_noise_level
        log.info(
            "init_audio: %d ch x %d frames @ %d Hz, init_noise_level=%.3f",
            _ia_tensor.shape[0] if _ia_tensor.dim() > 1 else 1,
            _ia_tensor.shape[-1],
            _ia_sr,
            init_noise_level,
        )
    if pipe.sampler_type:
        if pipe.sampler_type == "pingpong" and not _native_pingpong_supported():
            raise RuntimeError(_UNSUPPORTED_SAMPLER_MSG.format(
                model=pipe.model_name, sampler=pipe.sampler_type))
        generate_kwargs["sampler_type"] = pipe.sampler_type
    # SA3 (model_type "diffusion_cond_inpaint") generates through
    # generate_diffusion_cond_inpaint; SAO and friends use generate_diffusion_cond.
    # Both accept the same (conditioning_tensors, sample_size, sampler_type, seed,
    # device, steps, cfg_scale) interface, so the pre-computed cond_a and all the
    # embedding manipulation above are shared — only the entry point differs.
    model_type = pipe.model_config.get("model_type", "") if isinstance(pipe.model_config, dict) else ""
    # SAO models keep their short trained sample_size (~12s) and truncate to the
    # requested duration. SA3's trained sample_size is ~120s, so generating it in
    # full for a few-second request would waste minutes of compute — derive the
    # sample_size from the requested duration for the inpaint family (its rotary
    # DiT handles variable lengths; verified generating at sr*duration).
    if model_type == "diffusion_cond_inpaint":
        # The latent VAE floors audio_sample_size // downsampling_ratio, so a raw
        # ceil(duration*sr) that is not a whole number of latent frames yields
        # FEWER samples than requested (at 44.1k a 4.000s request floored to
        # 176128 = 3.994s) and the truncation below never fires. Round the
        # generation length UP so the model produces >= the request; the
        # truncation below then trims to exact.
        ds = int(getattr(getattr(pipe.model, "pretransform", None),
                         "downsampling_ratio", 1) or 1)
        # Resynth (init_audio) needs an EVEN latent block count. Measured: SA3's
        # VAE encoder rounds the latent length UP to the next even number
        # (encode of 32 blocks -> 32, 33 -> 34, 34 -> 34), but the library's
        # generate_diffusion_cond_inpaint sizes its noise latent as a plain
        # floor(audio_sample_size // ds). An ODD block count therefore makes the
        # encoded init_audio latent one frame longer than the noise latent, and
        # sample_diffusion's `init_data*(1-sigma)+noise*sigma` blend crashes on the
        # size mismatch. Aligning the resynth path up to an even block count
        # (2*ds) keeps both latents even and equal. Plain generation has no
        # init_data to blend, so it keeps its historical whole-block (ds) size and
        # existing seeds render identically.
        align = (2 * ds) if init_audio_tuple is not None else ds
        target = max(int(math.ceil(duration * sr)), 1)
        gen_sample_size = ((target + align - 1) // align) * align
    else:
        gen_sample_size = pipe.sample_size
    try:
        if model_type == "diffusion_cond_inpaint":
            from stable_audio_tools.inference.generation import generate_diffusion_cond_inpaint
            output = generate_diffusion_cond_inpaint(
                pipe.model,
                steps=effective_steps,
                cfg_scale=cfg_scale,
                conditioning_tensors=cond_a,
                sample_size=gen_sample_size,
                device=device,
                seed=seed,
                **generate_kwargs,
            )
        else:
            output = generate_diffusion_cond(
                pipe.model,
                steps=effective_steps,
                cfg_scale=cfg_scale,
                conditioning_tensors=cond_a,
                sample_size=gen_sample_size,
                device=device,
                seed=seed,
                **generate_kwargs,
            )
    finally:
        sys.stdout = real_stdout
        if restore_forward is not None:
            restore_forward()

    elapsed = time.time() - t0
    audio_np = output.squeeze(0).cpu().float().numpy()  # [channels, samples]
    requested_samples = int(math.ceil(duration * sr))
    if audio_np.shape[-1] > requested_samples:
        audio_np = audio_np[..., :requested_samples]

    # SA3 (diffusion_cond_inpaint) outputs hot — raw peak runs ~10x unity — so
    # peak-normalise to unit amplitude per the stable-audio-tools convention,
    # otherwise the plugin hard-clips. SAO's native output is left untouched to
    # preserve its historical levels (it sits near unity already).
    if model_type == "diffusion_cond_inpaint":
        out_peak = float(np.max(np.abs(audio_np))) if audio_np.size else 0.0
        if out_peak > 0.0:
            audio_np = audio_np / out_peak

    rms = float(np.sqrt(np.mean(audio_np ** 2)))
    peak = float(np.max(np.abs(audio_np)))
    log.info(f"Generated (native) in {elapsed:.1f}s, RMS={rms:.4f}, peak={peak:.4f}, shape={audio_np.shape}")
    emb_stats = (emb_a_pooled, emb_b_pooled, baseline_pooled)
    return audio_np, sr, seed, elapsed, emb_stats


def generate(pipe, request):
    """Run generation from request dict. Returns (audio_np, sample_rate, seed, elapsed, emb_stats).

    emb_stats = (emb_a_pooled[768], emb_b_pooled[768] or zeros)
    If "cache_as" is set in the request, the pre-VAE latent is cached for
    fast interpolation via interpolate_and_decode().
    """
    if isinstance(pipe, AudioLDM2Wrapper):
        return _generate_audioldm2(pipe, request)
    if isinstance(pipe, NativePipeline):
        return _generate_native(pipe, request)
    prompt_a = request.get("prompt_a", "")
    prompt_b = request.get("prompt_b", "")
    alpha = request.get("alpha", 0.0)
    magnitude = request.get("magnitude", 1.0)
    noise_sigma = request.get("noise_sigma", 0.0)
    duration = request.get("duration", 3.0)
    start_pos = request.get("start_pos", 0.0)
    steps = request.get("steps", 20)
    cfg_scale = request.get("cfg_scale", 7.0)
    seed = request.get("seed", -1)
    cache_as = request.get("cache_as")  # optional: cache latent under this name
    dim_offsets = _sanitize_dimension_offsets(request.get("dimension_offsets"))  # optional: [[idx, val], ...]
    injection_mode = request.get("injection_mode", "linear")
    if injection_mode != "linear":
        raise ValueError(
            f"injection_mode='{injection_mode}' is only implemented on the "
            "native pipeline (e.g. stable-audio-open-small); diffusers SAO "
            "currently only supports 'linear'."
        )

    if seed < 0:
        import random
        seed = random.randint(0, 2**31 - 1)

    sr = 44100
    generator = torch.Generator("cpu").manual_seed(seed)

    # Duration conditioning
    virtual_total = duration / (1.0 - start_pos) if start_pos < 1.0 else duration
    seconds_start = start_pos * virtual_total
    seconds_end = seconds_start + duration

    # Encode prompts via T5
    with torch.no_grad():
        tokenizer = pipe.tokenizer
        text_encoder = pipe.text_encoder

        def encode_text(text):
            inputs = tokenizer(text, return_tensors="pt", padding="max_length",
                               max_length=128, truncation=True)
            ids = inputs.input_ids.to(text_encoder.device)
            mask = inputs.attention_mask.to(text_encoder.device)
            out = text_encoder(input_ids=ids, attention_mask=mask)
            return out.last_hidden_state, mask

        a_present = bool(prompt_a)
        b_present = bool(prompt_b)

        # Always encode "" for the Spiegel-Punkt (used only for echo
        # derivation; CFG neg_embeds stays as torch.zeros_like below,
        # unchanged from the original behavior to keep CFG-driven sound
        # identical when both prompts are present).
        null_pe, null_mask = encode_text("")

        if a_present:
            emb_a, mask_a = encode_text(prompt_a)
        else:
            emb_a = null_pe.clone()
            mask_a = null_mask

        if b_present:
            emb_b, mask_b = encode_text(prompt_b)
        else:
            emb_b = null_pe.clone()
            mask_b = null_mask

        # Echoraum: a leeres Feld is the reflection of the other prompt
        # through null_pe (encoding of "").
        if a_present and not b_present:
            emb_b = _echo_through_null(emb_a, null_pe)
            mask_b = mask_a
        elif b_present and not a_present:
            emb_a = _echo_through_null(emb_b, null_pe)
            mask_a = mask_b

        # Mean-pool for DimensionExplorer stats (post-echo, pre-manipulation)
        emb_a_pooled = _mean_pool(emb_a, mask_a)
        emb_b_pooled = _mean_pool(emb_b, mask_b)

        combined_mask = (mask_a | mask_b)
        # alpha: 0 = midpoint, -1 = pure A, +1 = pure B; constant drive across
        # the whole range (see _blend_ab).
        manipulated = _blend_ab(emb_a, emb_b, alpha)

        # Magnitude scaling
        if abs(magnitude - 1.0) > 1e-6:
            manipulated = manipulated * magnitude

        # Noise injection (numpy PCG64 for cross-platform determinism)
        if noise_sigma > 0.0:
            rng = np.random.Generator(np.random.PCG64(seed))
            noise_np = rng.standard_normal(manipulated.shape).astype(np.float32)
            noise = torch.from_numpy(noise_np).to(manipulated.device, manipulated.dtype) * (noise_sigma * NOISE_SIGMA_SCALE)
            manipulated = manipulated + noise

        # Apply semantic axes
        sem_axes = request.get("semantic_axes")
        if sem_axes:
            manipulated = _apply_semantic_axes(manipulated, sem_axes, encode_text, "diffusers",
                                               amount=float(request.get("axes_amount", 1.0)))

        baseline_pooled = _mean_pool(manipulated, combined_mask)

        # Apply dimension offsets from DimensionExplorer last so they cannot be
        # overwritten by later conditioning operations.
        _apply_dimension_offsets(manipulated, dim_offsets)

        # Generate via pipeline with pre-computed embeddings
        neg_embeds = torch.zeros_like(manipulated)
        neg_mask = torch.ones_like(combined_mask)

        device = next(pipe.transformer.parameters()).device
        cache_str = f", cache_as='{cache_as}'" if cache_as else ""
        offsets_str = f", {len(dim_offsets)} offsets" if dim_offsets else ""
        log.info(f"Generating on {device}: '{prompt_a[:60]}' ({duration}s, {steps} steps, "
                 f"CFG={cfg_scale}, seed={seed}{cache_str}{offsets_str})")
        t0 = time.time()

        # If caching requested: get latent first, then decode separately
        output_type = "latent" if cache_as else "pt"

        result = pipe(
            prompt_embeds=manipulated,
            attention_mask=combined_mask,
            negative_prompt_embeds=neg_embeds,
            negative_attention_mask=neg_mask,
            audio_start_in_s=seconds_start,
            audio_end_in_s=seconds_end,
            num_inference_steps=steps,
            guidance_scale=cfg_scale,
            generator=generator,
            output_type=output_type,
        )

        if cache_as:
            # result.audios[0] is actually the raw latent when output_type="latent"
            latent = result.audios[0]
            if latent.dim() == 2:
                latent = latent.unsqueeze(0)  # ensure [1, C, T]
            _latent_cache[cache_as] = latent.cpu().clone()
            log.info(f"Cached latent '{cache_as}': {list(latent.shape)}")
            # Now VAE-decode for the audio response
            audio_np, _ = vae_decode(pipe, latent, duration)
        else:
            audio = result.audios[0]  # [channels, samples]
            if hasattr(audio, 'numpy'):
                audio_np = audio.cpu().float().numpy()
            else:
                audio_np = np.array(audio)
            # Trim to requested duration
            requested_samples = int(math.ceil(duration * sr))
            if audio_np.shape[-1] > requested_samples:
                audio_np = audio_np[..., :requested_samples]

        elapsed = time.time() - t0
        log.info(f"Generated in {elapsed:.1f}s")

        emb_stats = (emb_a_pooled, emb_b_pooled, baseline_pooled)
        return audio_np, sr, seed, elapsed, emb_stats


# ─── Protocol ───────────────────────────────────────────────────────

def send_ready(devices, default_device, models, default_model, model_metadata=None):
    """Signal ready to JUCE with device, model and per-model metadata.

    The optional ``model_metadata`` dict carries per-model static info that
    the plugin uses for UI clamping (DiT block count, default sampler type,
    etc.). Older plugin builds simply ignore the field, so adding it is
    backwards-compatible on the wire.
    """
    payload = {
        "devices": devices,
        "default": default_device,
        "models": list(models.keys()),
        "default_model": default_model,
    }
    if model_metadata:
        payload["model_metadata"] = model_metadata
    info = json.dumps(payload).encode("utf-8")
    sys.stdout.buffer.write(b'\x02')
    sys.stdout.buffer.write(struct.pack('<H', len(info)))
    sys.stdout.buffer.write(info)
    sys.stdout.buffer.flush()


def send_audio(audio_np, sr, seed, elapsed_ms, emb_stats=None):
    """Send audio response: \x01 + header + PCM [+ embedding stats].

    If emb_stats is (emb_a[num_dims], emb_b[num_dims], baseline[num_dims]), appends:
        uint16(num_dims) + float32[num_dims] emb_a
                         + float32[num_dims] emb_b
                         + float32[num_dims] baseline
    """
    channels, samples = audio_np.shape
    pcm = audio_np.astype(np.float32).tobytes()
    header = struct.pack('<iiiiif', 1, samples, channels, sr, seed, elapsed_ms * 1000)
    sys.stdout.buffer.write(b'\x01')
    sys.stdout.buffer.write(header)
    sys.stdout.buffer.write(pcm)
    if emb_stats is not None:
        emb_a, emb_b, baseline = emb_stats
        num_dims = len(emb_a)
        sys.stdout.buffer.write(struct.pack('<H', num_dims))
        sys.stdout.buffer.write(emb_a.astype(np.float32).tobytes())
        sys.stdout.buffer.write(emb_b.astype(np.float32).tobytes())
        sys.stdout.buffer.write(baseline.astype(np.float32).tobytes())
    else:
        sys.stdout.buffer.write(struct.pack('<H', 0))
    sys.stdout.buffer.flush()


def send_error(message):
    """Send error response: \x00 + length + UTF-8 message."""
    msg_bytes = message.encode('utf-8')
    sys.stdout.buffer.write(b'\x00')
    sys.stdout.buffer.write(struct.pack('<I', len(msg_bytes)))
    sys.stdout.buffer.write(msg_bytes)
    sys.stdout.buffer.flush()


def send_text(message):
    """Send text response: \x03 + uint32 length + UTF-8 message.

    Used by the optional ``translate`` mode. Mirrors the error-frame layout
    but with a distinct success status byte, so a client that issued a
    translate request distinguishes a translation result (\x03) from an
    error (\x00). Old clients never issue ``translate`` and so never receive
    a \x03 frame — adding it is backwards-compatible on the wire.
    """
    msg_bytes = (message or "").encode('utf-8')
    sys.stdout.buffer.write(b'\x03')
    sys.stdout.buffer.write(struct.pack('<I', len(msg_bytes)))
    sys.stdout.buffer.write(msg_bytes)
    sys.stdout.buffer.flush()


def send_partial(message):
    """Send an INTERIM frame: \x04 + uint32 length + UTF-8 message.

    Same layout as \x03, but it does not end the request — any number of these
    may precede the one frame that does. It exists so the Csound author's
    reasoning can be shown while it is being written instead of after, and it is
    sent from inside the generation, on the same thread that will send the final
    frame, so the two can never interleave.

    Only ever emitted for a request that asked for it, which today means
    mode="csound" with a client that reads \x04. Every other mode's bytes are
    unchanged, so a client that does not know this frame never receives one."""
    msg_bytes = (message or "").encode('utf-8')
    sys.stdout.buffer.write(b'\x04')
    sys.stdout.buffer.write(struct.pack('<I', len(msg_bytes)))
    sys.stdout.buffer.write(msg_bytes)
    sys.stdout.buffer.flush()


# ─── Main loop ──────────────────────────────────────────────────────

def _start_parent_watchdog(poll_seconds=2.0):
    """Exit when the plugin that launched this backend is gone.

    The request loop notices a dead parent only at stdin EOF, and it does not
    get back there while a request is in flight — so quitting the Standalone
    mid-generation orphaned this process indefinitely: it kept computing an
    answer nobody could receive any more. Measured 2026-07-22: one orphan four
    hours old, holding ~97% of a core with a 196 GB physical footprint and the
    machine's swap at 66.8 of 67.6 GB.

    Polling getppid() is what works DURING a request: a model load, a sampler
    loop or an LLM generation cannot be interrupted cooperatively from a
    thread, so the watchdog exits the process outright. os._exit skips
    interpreter cleanup on purpose — there is no one left to talk to, and
    atexit/GC on a half-finished MPS graph is exactly what we are escaping.

    Armed only when there IS a parent to watch: started detached (getppid()==1,
    a nohup/launchd run or a debugging session whose shell has gone) the
    watchdog would fire immediately, so it stays off.

    POSIX only, by construction: Windows does not reparent orphans, so
    getppid() there keeps returning the id of a parent that is long gone. The
    Windows guard is the kill-on-close job object the plugin puts the backend
    in (PipeInference.cpp), which needs nothing from this side.
    """
    import threading

    initial_ppid = os.getppid()
    if initial_ppid <= 1:
        log.info("no parent process to watch — orphan watchdog off")
        return

    def watch():
        while True:
            time.sleep(poll_seconds)
            if os.getppid() != initial_ppid:
                log.warning("parent process %d is gone — exiting", initial_ppid)
                try:
                    sys.stderr.flush()
                except Exception:
                    pass
                os._exit(0)

    threading.Thread(target=watch, name="parent-watchdog", daemon=True).start()


def main():
    global _available_models
    import sys
    sys.setrecursionlimit(50000)  # torchsde workaround

    # Before the startup model load: a Standalone quit during THAT orphans this
    # process just as thoroughly as one during a generation. (The module-level
    # torch import already ran; a parent that dies during it is caught anyway —
    # send_ready() then writes into a dead pipe and the process ends.)
    _start_parent_watchdog()

    _available_models = find_models()
    if not _available_models:
        if os.environ.get("T5YNTH_ALLOW_NO_MODELS") == "1":
            devices = available_devices()
            default_device = devices[0] if devices else "cpu"
            default_model = ""
            send_ready(devices, default_device, _available_models, "")
            log.warning("No model directories found; continuing in smoke-test mode")
        else:
            send_error("No model directories found")
            return
    else:
        try:
            devices = available_devices()
            default_model, default_device, startup_failures = choose_startup_model(_available_models, devices)
        except Exception as e:
            log.error(f"Failed to load default model: {e}")
            send_error(f"Pipeline load failed: {e}")
            return

        # Per-model metadata for the plugin's UI clamps (DiT block count,
        # sampler dispatch). Read straight from model_config.json so models
        # that haven't been loaded yet still show correct slider ranges.
        model_metadata = _collect_static_model_metadata(_available_models)
        send_ready(devices, default_device, _available_models, default_model,
                   model_metadata=model_metadata)
        log.info(f"Ready. Models: {list(_available_models.keys())}, default: {default_model}, "
                 f"devices: {devices}, default device: {default_device}")
        if model_metadata:
            log.info(f"Model metadata: {model_metadata}")
        if startup_failures:
            log.warning(f"Skipped unusable startup models: {startup_failures}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)

            # Optional prompt translation runs BEFORE audio-model routing: it
            # uses its own separately-installed model, so it must neither
            # require nor hard-fail on an audio model being present/installed.
            if request.get("mode") == "translate":
                t_device = request.get("device", default_device)
                if t_device == "auto" or t_device not in devices:
                    t_device = default_device
                source_text = request.get("prompt_a") or request.get("text") or ""
                send_text(run_author_instruct(request, source_text,
                                              TRANSLATION_SYSTEM_PROMPT, t_device))
                continue

            # Optional prompt INTERPRETATION: same instruct LLM as ``translate``,
            # but the caller supplies the system prompt — so the model can do more
            # than translate (vary, abduct a scene, contextualise, ...). Used by the
            # CLAP→LLM semantic-loop tooling ("CLAP is the ear, the LLM is the
            # interpreter"); like translate, it must not require an audio model.
            if request.get("mode") == "interpret":
                t_device = request.get("device", default_device)
                if t_device == "auto" or t_device not in devices:
                    t_device = default_device
                # Resolved the same way mode="csound" resolves it, so every
                # interpretation follows whatever the user loaded in Settings.
                # The retired `use_coder_model` flag once picked the author model
                # over the small translator; with one model there is nothing left
                # to pick, and neither side sends it any more.
                system_prompt = request.get("system_prompt") or TRANSLATION_SYSTEM_PROMPT
                source_text = request.get("prompt_a") or request.get("text") or ""
                # No default cap (see run_instruct): only an explicit request value
                # still limits the reply, and that one call site is C++-owned.
                _mn = request.get("max_new_tokens")
                max_new = int(_mn) if _mn is not None else None
                # No anti-cycling logit transforms. They existed to defeat the
                # degenerate token-cycle the 1.5B fell into on a long "recombine
                # these freely" palette ("sharp, thin, metallic, buzzing, sharp,
                # thin, …"), and they came at a real cost: no_repeat_ngram_size=3
                # spans the PROMPT as well as the output, so a system prompt that
                # spells out a required answer form forbade the model from ever
                # emitting that form — the self-check's "asked for X, but the sound
                # measures Y" came back as "butthe sound measuresbrightandthin",
                # with the repetition penalty pushing into rare tokens ("noise洗").
                # Measured 2026-07-22 on the author model, all seven stances (the
                # real system prompts and user turns, over this wire): zero
                # repeated 3-grams either way, the self-check's required form
                # intact, and every reply identical with the transforms on and off.
                # The pathology was the small model, not the task.
                send_text(run_author_instruct(request, source_text, system_prompt,
                                              t_device, max_new))
                continue

            # The LCO: the LLM WRITES the Csound orchestra. One prompt, one
            # inference against the curated library of real idioms
            # (backend/lco_write.py), one orchestra, and it runs. Morph, loop,
            # mix and motion are IN the emitted code because the model writes
            # them -- Csound is a programming language, and a loop is code.
            # Python assembles the prompt, wraps the body in the host scaffold
            # and lets the compiler judge it; a failure goes back to the model
            # with Csound's own error. No audio-model dependency, dispatched
            # before audio-model routing like translate/interpret; a missing
            # author model raises the standard error frame. LLM-FIRST, NO
            # FALLBACK -- no model, no oscillator, never a prefabricated tone.
            if request.get("mode") == "csound":
                from lco_write import build_csound_response

                t_device = request.get("device", default_device)
                if t_device == "auto" or t_device not in devices:
                    t_device = default_device
                author_backend = _resolve_author_backend(request)
                if author_backend[0] == "unavailable":
                    raise RuntimeError("Csound author model not installed (load it in Settings)")
                # Only the local_transformers branch needs a device at all —
                # computed lazily so this stays a no-op for the GGUF and both
                # external-API branches instead of implying they run "on" a
                # torch device.
                coder_device = (_translator_device(t_device)
                                if author_backend[0] == "local_transformers" else None)

                def csound_llm(text, system_prompt, max_new_tokens, on_delta=None,
                               _backend=author_backend, _dev=coder_device):
                    """The single model surface build_csound_response needs: one
                    greedy instruct call on the author model. Four shapes reach
                    here — the shipped 4-bit GGUF through llama.cpp, a
                    transformers directory (dev drop, or a pre-GGUF install), or
                    an external OpenAI-compatible / Anthropic API.

                    llama.cpp and both API branches stream. On the transformers
                    path `on_delta` is dropped and the reasoning arrives with the
                    final frame, as it did before — the shipped local author is
                    the GGUF, and a HuggingFace streamer needs its own thread to
                    do the same."""
                    kind = _backend[0]
                    # try/finally, not a plain call: one authoring is a WHOLE
                    # repair loop, and a round that ends in an exception was
                    # still billed. Accounting only what succeeded would
                    # under-report exactly the expensive case.
                    if kind == "api_openai_compat":
                        _, base_url, api_key, model = _backend
                        try:
                            return call_openai_compatible(base_url, api_key, model, text,
                                                          system_prompt,
                                                          max_new_tokens=max_new_tokens,
                                                          on_delta=on_delta)
                        finally:
                            _account_api_call()
                    if kind == "api_anthropic":
                        _, api_key, model = _backend
                        try:
                            return call_anthropic(api_key, model, text, system_prompt,
                                                  max_new_tokens=max_new_tokens,
                                                  on_delta=on_delta)
                        finally:
                            _account_api_call()
                    if kind == "local_gguf":
                        _, gguf, _model_dir = _backend
                        return run_gguf_instruct(text, gguf, system_prompt,
                                                 max_new_tokens=max_new_tokens,
                                                 on_delta=on_delta)
                    _, model_dir = _backend  # local_transformers
                    return run_instruct(text, model_dir, _dev, system_prompt,
                                        max_new_tokens=max_new_tokens)

                # Both shapes take a conversation, so a repair may CONTINUE the
                # one that failed instead of starting the sound over. Declared,
                # not inferred: build_csound_response only sends a message list
                # to a surface that says it takes one.
                csound_llm.accepts_messages = True

                # The authoring, live. Gated on the request so a client that does
                # not know the \x04 frame never sees one: the backend ships with
                # the plugin, but during development the two versions mix freely,
                # and an unrecognised frame byte would desynchronise the pipe for
                # the rest of the session, not just for this request. Three kinds
                # (§4.6): the reasoning, the code as it is written, and each
                # repair round as it starts — a client ignores kinds it does not
                # know, so the three degrade independently.
                on_thinking = None
                on_body = None
                on_attempt = None
                if request.get("stream"):
                    def on_thinking(attempt, thinking):
                        send_partial(json.dumps({"kind": "thinking",
                                                 "attempt": attempt,
                                                 "text": thinking}))

                    # The token output itself. The code is most of the
                    # generation, and it was the invisible part: the panel went
                    # dark the moment the reasoning ended and stayed dark for
                    # minutes (BJ 2026-07-24).
                    def on_body(attempt, code):
                        send_partial(json.dumps({"kind": "body",
                                                 "attempt": attempt,
                                                 "text": code}))

                    # A repair round starting: attempt counter, ceiling, and
                    # every distinct compiler error the author is being shown.
                    # Each round regenerates the whole body — without this frame
                    # those minutes are captioned as attempt 1.
                    def on_attempt(attempt, max_tries, errors):
                        send_partial(json.dumps({"kind": "attempt",
                                                 "attempt": attempt,
                                                 "max": max_tries,
                                                 "errors": errors}))

                # DEPRECATED (LCO self-check deactivated 2026-07-21): the product no
                # longer sends `correction`/`previous` — the C++ bake stopped running
                # the self-listen / compare / correct loop (PromptPanel.cpp
                # T5YNTH_LCO_SELFCHECK = 0). These .get()s stay for wire compatibility
                # but now resolve to "".
                # An author that RAISES (a 401 on a stale key, a 429 mid repair
                # loop, a stream the provider dropped) is turned into the same
                # {"ok": false, "error": ...} shape the write-path returns for a
                # body it could not fix, instead of propagating to the outer
                # handler's \x00 error frame. Two reasons, one of them the whole
                # point of this block: the \x00 frame is a bare string and cannot
                # carry the api_spend below, so an authoring that billed four
                # repair rounds before dying would report nothing spent —
                # precisely the run the user most wants accounted. And the
                # client sees no difference: PipeInference::authorCsoundOrchestra
                # sets the same errorMessage from either frame, as does
                # tools/lco_trace_wire_check.py.
                try:
                    response = build_csound_response(request.get("text") or "", csound_llm,
                                                     request.get("correction") or "",
                                                     request.get("previous") or "",
                                                     on_thinking=on_thinking,
                                                     on_body=on_body,
                                                     on_attempt=on_attempt,
                                                     synth_params=request.get("synth_params"))
                except Exception as exc:
                    log.error(f"csound authoring failed: {exc}")
                    # The traceback the outer handler would have written. Kept:
                    # backend_stderr.log is what a user pastes into a support
                    # request, and intercepting the exception here must not make
                    # this one failure class quieter than every other.
                    import traceback
                    traceback.print_exc(file=sys.stderr)
                    response = {"ok": False, "error": str(exc)}
                # Report WHICH model actually authored. The UI otherwise shows a
                # hardcoded name and cannot tell that the resolver walked past the
                # intended slot: an install missing its shard map fails
                # _is_local_transformers_model_dir, the scan continues, and another
                # coder writes the orchestra while the panel still names the first.
                # Observed exactly so (2026-07-22): the 7B slot was unloadable and
                # the 3B authored, with nothing on the wire to show it.
                if isinstance(response, dict):
                    response["author_model"] = _author_backend_label(author_backend)
                    # What the external author has cost so far, this session.
                    # Ridden along on THIS response and no other because it is
                    # the only one with a JSON envelope — translate/interpret
                    # answer with bare text, and giving them a wrapper to carry a
                    # counter would change the wire for every existing client.
                    # No loss: the counter is cumulative and process-wide, so
                    # their tokens appear here too, at the next authoring.
                    # Attached on failure too — see the try/except above.
                    if _API_SPEND["calls"] > 0:
                        response["api_spend"] = api_spend_snapshot()
                send_text(json.dumps(response))
                continue

            # A body the USER edited, compiled without a model. Same host
            # scaffold, same compiler, same error text as authoring — it is the
            # authoring path with the author replaced by a person, which is the
            # whole point of making the back of the card editable. Dispatched
            # here beside mode="csound" because it shares its dependencies:
            # Csound, and nothing else.
            if request.get("mode") == "csound_compile":
                from lco_write import compile_body

                send_text(json.dumps(compile_body(request.get("body") or "")))
                continue

            # CLAP machine-listening analysis: decode init_audio → top-k CLAP tags
            # + DSP spectral words, returned in ONE \x03 text frame as JSON. Like
            # translate/interpret it is dispatched BEFORE audio-model routing — it
            # has NO audio-model dependency (CLAP is its own CPU-pinned model). It
            # is the ear half of the CLAP→LLM semantic loop (interpret = the LLM
            # half). Errors (bad/absent audio, CLAP sanity failure) propagate to the
            # standard \x00 error frame via the outer try/except.
            if request.get("mode") == "analyze":
                send_text(analyze_audio(request))
                continue

            # Route to correct model + device.
            # An explicitly requested model that is not installed MUST fail loud.
            # Silently falling back to default_model masks a missing/mis-installed
            # model: the user selects e.g. SA3, its files are absent, and they
            # unknowingly hear the default model run with the requested model's
            # sampling params — which sounds like the selected model is broken.
            requested_model = request.get("model")
            device = request.get("device", default_device)
            if requested_model is None:
                model = default_model
            elif requested_model in _available_models:
                model = requested_model
            else:
                raise ValueError(
                    f"Model '{requested_model}' is not installed "
                    f"(available: {sorted(_available_models)}). "
                    f"Re-run model setup to install it."
                )
            if device == "auto" or device not in devices:
                device = default_device

            mode = request.get("mode", "generate")

            if mode == "preload":
                # Just load the pipeline (lazy-load cache), respond with empty audio
                log.info(f"Preloading {model} on {device}...")
                pipe = get_pipeline(model, device)
                log.info(f"Preload complete: {model} on {device}")
                # Send minimal success response (0 samples)
                empty = np.zeros((2, 0), dtype=np.float32)
                send_audio(empty, 44100, 0, 0.0)
                continue

            pipe = get_pipeline(model, device)
            emb_stats = None

            if mode == "interpolate":
                if isinstance(pipe, AudioLDM2Wrapper) or not hasattr(pipe, "vae"):
                    raise ValueError("Latent interpolation is only supported for diffusers Stable Audio pipelines")
                audio, sr, seed, elapsed = interpolate_and_decode(pipe, request)
            elif mode == "decode_cached":
                if isinstance(pipe, AudioLDM2Wrapper) or not hasattr(pipe, "vae"):
                    raise ValueError("Latent cache decode is only supported for diffusers Stable Audio pipelines")
                audio, sr, seed, elapsed = decode_cached(pipe, request)
            else:
                audio, sr, seed, elapsed, emb_stats = generate(pipe, request)

            send_audio(audio, sr, seed, elapsed, emb_stats)
        except Exception as e:
            log.error(f"Request failed (model={request.get('model', '?')}, "
                      f"mode={request.get('mode', 'generate')}): {e}")
            import traceback
            traceback.print_exc(file=sys.stderr)
            send_error(str(e))


if __name__ == "__main__":
    main()
    # main() returns only on a clean shutdown (the host closed stdin). Leave via
    # os._exit so the process does NOT run C++ static destructors.
    #
    # llama.cpp's Metal backend aborts in one: ggml-metal-device.m asserts
    # `[rsets->data count] == 0` while freeing the device at __cxa_finalize, so a
    # backend that has authored even one orchestra exits 134/SIGABRT instead of 0
    # (measured; upstream ggml-org/llama.cpp#17869). The host would read that as a
    # backend crash on every ordinary quit. Nothing here owns unflushed state --
    # every frame is written and flushed as it is produced -- so skipping
    # finalization costs nothing. Error paths inside main() still exit their own
    # way and keep their status.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0)
