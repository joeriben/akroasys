#!/usr/bin/env python3
"""Is the regenerate harness measuring the preset it claims to?

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_wirecheck.py
    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_wirecheck.py local

Compares the buffer the harness got for a preset's saved request with the buffer the
preset stores. Same model, prompts and seed should give nearly the same audio. It need
not be bit-exact: the preset may have been rendered on another device or with an earlier
backend.
"""

import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from pitch_c3_bank import payloads, read_preset
from pitch_c3_generate import PRESETS
from pitch_c3_ipc import STUDY_DIR
import io



def local_check():
    """All local presets, unnamed: their prompts and audio are private. How many stored
    buffers does the harness reproduce, and does a request repeat itself? A preset saved
    while drift or Resynth ran stores a state its request cannot restate."""
    from pitch_c3_generate import preset_request
    from pitch_c3_ipc import Backend

    def corr(a, b):
        m = min(len(a), len(b))
        x = np.fft.irfft(np.fft.rfft(a[:m], 2 * m) * np.conj(np.fft.rfft(b[:m], 2 * m)), 2 * m)
        return float(x[np.argmax(np.abs(x))] / (np.linalg.norm(a[:m]) * np.linalg.norm(b[:m]) + 1e-12))

    be = Backend(None)
    found = []
    for path in (Path.home() / "Library/T5ynth/presets").glob("*.t5p"):
        version, meta, tail = read_preset(path)
        req = None
        if (meta.get("audio_meta") or {}).get("numSamples") and (meta.get("synth") or {}).get("model"):
            req = preset_request(meta, be.ready, be.ready["default"])
        if req is not None and req["model"] in be.ready["models"]:
            found.append((meta.get("timestamp", ""), version, meta, tail, req))
    found.sort(key=lambda f: f[0], reverse=True)
    against, repeats = [], []
    for i, (_t, version, meta, tail, req) in enumerate(found):
        stored, _sr = sf.read(io.BytesIO(payloads(version, meta, tail)[0][0]), dtype="float32", always_2d=True)
        try:
            a, _, _, _ = be.request(dict(req))
            against.append(corr(stored.mean(1), a.mean(0)))
            if i < 5:
                b, _, _, _ = be.request(dict(req))
                repeats.append(corr(a.mean(0), b.mean(0)))
        except RuntimeError as exc:
            print(f"request {i} failed: {exc}", file=sys.stderr)
    be.close()
    v = np.array(against)
    print(f"{len(v)} local presets with stored audio and an installed model: {int(np.sum(v >= 0.9))} reproduce their "
          f"stored buffer at correlation 0.9 or above, {int(np.sum((v >= 0.5) & (v < 0.9)))} between 0.5 and 0.9, "
          f"{int(np.sum(v < 0.5))} below. The same request twice, {len(repeats)} presets: lowest correlation "
          f"{min(repeats):+.4f}.")


if len(sys.argv) > 1 and sys.argv[1] == "local":
    local_check()
    sys.exit()

root = STUDY_DIR / "regen"
rows = [json.loads(l) for l in (root / "index.jsonl").open(encoding="utf-8")]
seen = set()
print("| Preset | Stored s | Harness s | Peak correlation | Lag ms | Stored pitch | Harness pitch |")
print("|---|---:|---:|---:|---:|---|---|")
import pitch_c3_f0 as P
for r in rows:
    if r["regime"] != "saved" or r["preset"] in seen:
        continue
    seen.add(r["preset"])
    version, meta, tail = read_preset(PRESETS / f"{r['preset']}.t5p")
    stored_blobs, has_audio = payloads(version, meta, tail)
    if not has_audio:
        print(f"| {r['preset']} | stores no audio | | | | | |")
        continue
    stored, sr = sf.read(io.BytesIO(stored_blobs[0]), dtype="float32", always_2d=True)
    made, _ = sf.read(root / r["path"], dtype="float32", always_2d=True)
    a, b = P.to_mono(stored), P.to_mono(made)
    n = min(len(a), len(b))
    fa, fb = np.fft.rfft(a[:n], 2 * n), np.fft.rfft(b[:n], 2 * n)
    x = np.fft.irfft(fa * np.conj(fb), 2 * n)
    k = int(np.argmax(np.abs(x)))
    corr = x[k] / (np.linalg.norm(a[:n]) * np.linalg.norm(b[:n]) + 1e-12)
    lag = (k if k < n else k - 2 * n) / sr * 1000
    ya, yb = P.shipped_yin(a, sr), P.shipped_yin(b, sr)
    name = lambda s: "–" if s["dominant_midi"] is None else P.note_name(s["dominant_midi"])
    print(f"| {r['preset']} | {len(a) / sr:.2f} | {len(b) / sr:.2f} | {corr:+.3f} | {lag:.1f} | {name(ya)} | {name(yb)} |")
