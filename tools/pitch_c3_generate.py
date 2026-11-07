#!/usr/bin/env python3
"""Generate the material of the c3 pitch study through the frozen backend.

    .venv/bin/python tools/pitch_c3_generate.py matrix [--engines a,b] [--seeds 16]
    .venv/bin/python tools/pitch_c3_generate.py regen

Work order: docs/plans/WORKORDER_PITCH_NORMALISATION_C3.md, questions 1 and 3.

The design is fixed here, before anything was measured, so a result cannot pick
its own prompts:

* PITCHED: ten prompts the bank carries verbatim with `, c3`, one sound source
  each, chosen to cover waveforms, sustained, plucked, struck and sung sources.
* CONDITIONS: the bare prompt, then `, c3`, `, c2`, `, c4`, and `, g3`. The bank
  uses `, g3` itself; here it is the control the order does not list but its
  question needs: a share "on C" cannot say the model read the letter unless a
  different letter moves the result.
* The same seeds in every condition, so a cell differs from its neighbour by the
  suffix and nothing else.
* CONTROLS: sources without a pitch, for the false-alarm side of question 5.

Audio goes to STUDY_DIR, outside the repository. Resumable: an existing file is
not generated again.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import re
import struct
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from pitch_c3_ipc import STUDY_DIR, Backend, base_request, default_params_for

PITCHED = [
    "steady saw wave", "steady square wave", "Hammond organ", "a violin", "cello",
    "a trombone", "sitar", "tubular bells", "a fog horn",
    "male opera baritone voice singing",
]
CONTROLS = [
    "rain on a tin roof", "glass breaking", "crackling fire", "paper tearing",
    "a door slamming", "helicopter rotor", "a radio discussion",
    "laughing hysterically", "a samba group", "calm waves",
]
CONDITIONS = {"none": "", "c3": ", c3", "c2": ", c2", "c4": ", c4", "g3": ", g3"}
CONTROL_CONDITIONS = ("none", "c3")
SEED_BASE = 1000

# name in this study -> (model id, track_type). The SFX domain is the same medium
# checkpoint with track_type "sfx"; the plugin persists it as "...-sfx".
ENGINES = {
    "sao-small": ("stable-audio-open-small", None),
    "sa3-small-music": ("stable-audio-3-small-music", "instrument"),
    "sa3-medium": ("stable-audio-3-medium", "instrument"),
    "sa3-medium-sfx": ("stable-audio-3-medium", "sfx"),
    "sao-1.0": ("stable-audio-open-1.0", None),
    "audioldm2": ("audioldm2", None),
}


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def write_clip(path, audio: np.ndarray, sr: int) -> float:
    """24-bit FLAC. Models overshoot 1.0 (SAO 1.0 does), FLAC would clip, and level
    is not what is measured, so a clip above full scale is scaled down; the true peak
    goes in the index."""
    peak = float(np.abs(audio).max())
    scale = 1.0 / peak if peak > 1.0 else 1.0
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp.flac")
    sf.write(tmp, (audio * scale).T, sr, subtype="PCM_24", format="FLAC")
    tmp.rename(path)
    return peak


def run_matrix(engines: list[str], seeds: int, control_seeds: int) -> None:
    root = STUDY_DIR / "matrix"
    root.mkdir(parents=True, exist_ok=True)
    index = open(root / "index.jsonl", "a", encoding="utf-8")
    be = Backend(STUDY_DIR / "backend_stderr.log")
    device = be.ready["default"]
    for name in engines:
        model, track = ENGINES[name]
        if model not in be.ready["models"]:
            print(f"SKIP {name}: {model} not installed", flush=True)
            continue
        cells = [(p, c, "pitched", s) for p in PITCHED for c in CONDITIONS for s in range(seeds)]
        cells += [(p, c, "control", s) for p in CONTROLS for c in CONTROL_CONDITIONS
                  for s in range(control_seeds)]
        t0, done = time.time(), 0
        for prompt, cond, group, s in cells:
            seed = SEED_BASE + s
            path = root / name / slug(prompt) / f"{cond}__s{seed}.flac"
            if path.exists():
                continue
            text = prompt + CONDITIONS[cond]
            audio, sr, real_seed, ms = be.request(
                base_request(model, device, text, seed=seed, track_type=track))
            peak = write_clip(path, audio, sr)
            index.write(json.dumps({
                "engine": name, "model": model, "track_type": track, "group": group,
                "prompt": prompt, "condition": cond, "text": text, "seed": real_seed,
                "path": str(path.relative_to(root)), "peak": peak, "ms": ms,
                "samples": audio.shape[1]}, ensure_ascii=False) + "\n")
            index.flush()
            done += 1
            if done % 100 == 0:
                print(f"{name}: {done} new, {(time.time() - t0) / done:.2f} s each", flush=True)
        print(f"{name}: finished, {done} new in {time.time() - t0:.0f} s", flush=True)
    be.close()


# ── Question 3: regenerate a preset the way the player does ──────────────────

PRESETS = Path.home() / "Library/T5ynth/presets/UCDCAE AI Lab"
# Published bank presets only, so the report can name them: pitched sources, one per
# engine at least, and one that carries its own alpha drift.
REGEN_PRESETS = [
    "Dirty Squarewave", "Funky Zing", "Vintage Saw Gran", "evolvement03 SA3",
    "Echoes of a Laughing Kalimba Gran", "Saw a Horseback 140bpm",
]
REGENS = 10
REGEN_SEED_BASE = 2000
# The stand-in for a preset with no LFO on alpha. Depth 0.2 triangle is what the bank's
# own alpha LFOs hold (0.05 to 0.2); it is an assumption and the report says so.
STANDIN_LFO = {"waveform": "triangle", "depth": 0.2}
WIDE_DEPTH = 0.5   # alpha +-1: from pole to pole around the preset's own position
PHASE_STEP = 0.381966011   # 1 - 1/golden ratio

# AxesPanel.cpp kAxesSAO / kAxesSA3: dropdown id -> backend key. Id 1 is "---".
AXIS_KEYS = {2: "music_noise", 3: "acoustic_electronic", 4: "improvised_composed",
             5: "refined_raw", 6: "solo_ensemble", 7: "sacred_secular", 8: "tonal_noisy",
             9: "rhythmic_sustained", 10: "rhythmic_sustained_sa3",
             11: "grainy_smooth_sa3", 12: "dense_sparse_sa3"}


def lfo_value(waveform: str, p: float) -> float:
    """DriftLFO::waveformValue for phase p in [0, 1)."""
    if waveform == "sine":
        return math.sin(2 * math.pi * p)
    if waveform == "triangle":
        return 4 * p if p < 0.25 else (2 - 4 * p if p < 0.75 else 4 * p - 4)
    if waveform == "sawtooth":
        return 2 * p - 1
    if waveform == "square":
        return 1.0 if p < 0.5 else -1.0
    if waveform == "random":   # sample & hold: a new value per step, drawn reproducibly
        return float(np.random.default_rng(int(p * 1000)).uniform(-1.0, 1.0))
    raise ValueError(f"waveform {waveform} is not modelled here")


def load_preset(name: str) -> tuple[dict, bytes]:
    data = (PRESETS / f"{name}.t5p").read_bytes()
    assert data[:4] == b"T5YN", name
    (json_len,) = struct.unpack("<I", data[8:12])
    return json.loads(data[12:12 + json_len].decode("utf-8")), data[12 + json_len:]


def preset_request(meta: dict, ready: dict, device: str, alpha_off: float = 0.0) -> dict:
    """What PromptPanel::buildInferenceRequest sends for this preset, alpha_off being the
    drift offset. Linear takes it on alpha, step and kombi modes on the late mix
    (x 0.25), layer on the split position: pollDriftRegen's routing."""
    s = meta["synth"]
    tagged = s.get("model") or ""
    sfx = tagged.endswith("-sfx") and tagged != "stable-audio-3-small-sfx"
    model = tagged[:-4] if sfx else tagged
    steps, cfg = default_params_for(model)
    mode = s.get("injectionMode") or "linear"
    pa, pb = (s.get("promptA") or "").strip(), (s.get("promptB") or "").strip()
    alpha = float(s.get("alpha") or 0.0)
    late = float(s.get("lateMixAmount") or 0.0)
    fine = mode in ("late_step", "kombi1", "kombi2", "kombi3")
    if mode == "linear":
        alpha += alpha_off
        if alpha_off == 0.0 and bool(pa) != bool(pb):
            alpha = -1.0 if pa else 1.0
    elif fine:
        late = min(1.0, max(0.0, late + alpha_off * 0.25))
    blocks = float((ready.get("model_metadata") or {}).get(model, {}).get("dit_blocks") or 16)
    split_start = min(blocks, max(0.0, float(s.get("splitStart") or 4.0)))
    split_end = min(blocks, max(0.0, float(s.get("splitEnd") or 16.0)))
    if mode == "layer_split":
        width = split_end - split_start
        split_start = min(max(0.0, blocks - width), max(0.0, split_start + alpha_off * blocks * 0.5))
        split_end = split_start + width
    if mode in ("kombi1", "kombi2", "kombi3"):
        split_start, split_end = {"kombi1": (0.0, 0.25), "kombi2": (0.25, 0.75),
                                  "kombi3": (0.375, 0.625)}[mode]
        split_start, split_end = split_start * blocks, split_end * blocks
    t = min(1.0, max(0.0, late))
    req = {
        "prompt_a": pa, "prompt_b": pb, "alpha": alpha,
        "magnitude": float(s.get("magnitude") or 1.0), "noise_sigma": float(s.get("noise") or 0.0),
        "duration": float(s.get("duration") or 3.0), "start_pos": 0.0,
        "steps": steps, "cfg_scale": cfg, "seed": int(s.get("seed", -1)),
        "device": device, "model": model,
        "modality_epoch": int(s.get("modalityEpoch", 0)),
        "injection_mode": "linear" if "audioldm2" in model else mode,
        "injection_transition_at": min(0.95, max(0.05, 0.5 - 0.45 * t)),
        "late_phase_alpha": t, "split_start": split_start, "split_end": split_end,
    }
    if "stable-audio-3" in model:
        req["track_type"] = "sfx" if sfx else "instrument"
    axes = {AXIS_KEYS[a["dropdownId"]]: float(a["value"])
            for a in meta.get("semanticAxes", []) if a.get("dropdownId", 1) in AXIS_KEYS}
    if axes:
        req["semantic_axes"] = axes
        req["axes_amount"] = float(s.get("axesAmount", 1.0))
    return req


def alpha_lfos(meta: dict) -> tuple[list[dict], bool]:
    """All LFOs on alpha: DriftLFO::getOffsetForTarget sums them. Drift on an axis is not
    modelled here."""
    own = [l for l in meta.get("driftLfos", []) if l.get("target") == "alpha" and l.get("depth", 0) > 0.0]
    return (own, True) if own else ([STANDIN_LFO], False)


def drift_offset(lfos: list[dict], i: int) -> float:
    """The alpha offset at regenerate i. Phases step by the golden section, so no two
    regenerates ask for the same offset: pollDriftRegen does not regenerate at a fixed
    seed when nothing moved, and a repeat would count as a step of zero. Several LFOs start
    a quarter cycle apart. halfRange of alpha is 2."""
    return sum(lfo_value(l["waveform"], (i * PHASE_STEP + k * 0.25) % 1.0) * float(l["depth"]) * 2.0
               for k, l in enumerate(lfos))


def run_regen(names: list[str], wide_only: bool = False) -> None:
    root = STUDY_DIR / "regen"
    root.mkdir(parents=True, exist_ok=True)
    index = open(root / "index.jsonl", "a", encoding="utf-8")
    be = Backend(STUDY_DIR / "backend_stderr_regen.log")
    device = be.ready["default"]
    for name in names:
        try:
            (regen_wide if wide_only else regen_one)(be, device, index, root, name)
        except RuntimeError as exc:
            print(f"FAILED {name}: {exc}", flush=True)
    be.close()


def regen_wide(be, device, index, root, name) -> None:
    meta, _tail = load_preset(name)
    for i in range(REGENS):
        off = drift_offset([{"waveform": "triangle", "depth": WIDE_DEPTH}], i)
        req = preset_request(meta, be.ready, device, off)
        audio, sr, seed, ms = be.request(req)
        rel = f"{slug(name)}/fixed_seed_wide_drift/{i:02d}.flac"
        peak = write_clip(root / rel, audio, sr)
        index.write(json.dumps({"preset": name, "regime": "fixed_seed_wide_drift", "i": i,
                                "alpha_off": off, "seed": seed, "peak": peak, "ms": ms, "path": rel,
                                "own_alpha_lfo": False, "lfo": {"waveform": "triangle", "depth": WIDE_DEPTH},
                                "request": req}, ensure_ascii=False) + "\n")
        index.flush()


def regen_one(be, device, index, root, name) -> None:
    if True:
        meta, _tail = load_preset(name)
        lfos, own_lfo = alpha_lfos(meta)
        base = preset_request(meta, be.ready, device)
        if base["model"] not in be.ready["models"]:
            print(f"SKIP {name}: {base['model']} not installed", flush=True)
            return
        is_sa3 = "stable-audio-3" in base["model"]

        def emit(regime, i, req, alpha_off):
            audio, sr, seed, ms = be.request(req)
            rel = f"{slug(name)}/{regime}/{i:02d}.flac"
            peak = write_clip(root / rel, audio, sr)
            shown = {k: v for k, v in req.items() if k != "init_audio_b64"}
            index.write(json.dumps({"preset": name, "regime": regime, "i": i, "alpha_off": alpha_off,
                                    "seed": seed, "peak": peak, "ms": ms, "path": rel,
                                    "own_alpha_lfo": own_lfo, "lfo": lfos, "request": shown},
                                   ensure_ascii=False) + "\n")
            index.flush()
            return audio, sr

        # The saved request itself, once: the wire check. The buffer it returns has to
        # match what the preset stores, or this harness measures another patch.
        emit("saved", 0, dict(base), 0.0)

        for i in range(REGENS):
            off = drift_offset(lfos, i)
            emit("fixed_seed_alpha_drift", i, preset_request(meta, be.ready, device, off), off)
            r = dict(base, seed=REGEN_SEED_BASE + i)
            emit("random_seed", i, r, 0.0)
            r = dict(preset_request(meta, be.ready, device, off), seed=REGEN_SEED_BASE + i)
            emit("random_seed_alpha_drift", i, r, off)

        for i in range(REGENS):   # the same, across most of the A/B travel
            off = drift_offset([{"waveform": "triangle", "depth": WIDE_DEPTH}], i)
            emit("fixed_seed_wide_drift", i, preset_request(meta, be.ready, device, off), off)

        if is_sa3:   # Resynth, internal: each render is seeded by the last raw output
            for amount in (0.25, 0.75):
                prev = None
                for i in range(REGENS):
                    r = dict(base, seed=REGEN_SEED_BASE + i)
                    if prev is not None:
                        audio, sr = prev
                        r.update({"init_audio_b64": base64.b64encode(
                                      np.ascontiguousarray(audio, dtype="<f4").tobytes()).decode("ascii"),
                                  "init_audio_sr": sr, "init_audio_channels": audio.shape[0],
                                  "init_noise_level": 0.90 - 0.85 * amount})
                    prev = emit(f"resynth_{int(amount * 100)}", i, r, 0.0)
        print(f"{name}: done", flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["matrix", "regen"])
    ap.add_argument("--engines", default=",".join(ENGINES))
    ap.add_argument("--seeds", type=int, default=16)
    ap.add_argument("--control-seeds", type=int, default=8)
    ap.add_argument("--presets", default="|".join(REGEN_PRESETS))
    ap.add_argument("--wide-only", action="store_true")
    a = ap.parse_args()
    if a.what == "regen":
        run_regen(a.presets.split("|"), a.wide_only)
    else:
        run_matrix(a.engines.split(","), a.seeds, a.control_seeds)
