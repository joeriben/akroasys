#!/usr/bin/env python3
"""Calibrate the f0 meter on signals whose pitch is known by construction.

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_calibrate.py

A meter that nobody calibrated returns numbers, not errors. Every signal here has its
true pitch set by the generator, or has no pitch at all, and the table says what each
estimator made of it. Output: STUDY_DIR/calibration.json and a printed table.
"""

from __future__ import annotations

import json
import sys
import zlib

import numpy as np

import pitch_c3_f0 as P
from pitch_c3_ipc import STUDY_DIR

SR = 44100
DUR = 3.0
CREPE_DEVICE = "mps"
PITCHES = [24, 36, 48, 60, 72, 84, 39.3, 51.7, 57.0, 66.2, 79.6]  # C1..C6 and off-grid


def additive(hz, amps, sr=SR, dur=DUR, partial_ratios=None, decays=None, rng=None):
    n = int(sr * dur)
    t = np.arange(n) / sr
    y = np.zeros(n)
    ratios = partial_ratios if partial_ratios is not None else np.arange(1, len(amps) + 1)
    for i, (a, r) in enumerate(zip(amps, ratios)):
        if hz * r >= sr / 2:
            continue
        ph = rng.uniform(0, 2 * np.pi) if rng is not None else 0.0
        part = a * np.sin(2 * np.pi * hz * r * t + ph)
        if decays is not None:
            part = part * np.exp(-t * decays[i])
        y += part
    return y


def pink(n, rng):
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n)
    f[0] = f[1]
    return np.fft.irfft(spec / np.sqrt(f), n)


def norm(y):
    return (0.5 * y / np.max(np.abs(y))).astype(np.float32)


def make(kind: str, midi: float, rng) -> tuple[np.ndarray, str]:
    hz = float(P.midi_to_hz(midi))
    n = int(SR * DUR)
    k = np.arange(1, 81)
    if kind == "sine":
        return norm(additive(hz, [1.0], rng=rng)), "pitched"
    if kind == "saw":
        return norm(additive(hz, 1.0 / k, rng=rng)), "pitched"
    if kind == "square":
        return norm(additive(hz, np.where(k % 2 == 1, 1.0 / k, 0.0), rng=rng)), "pitched"
    if kind == "missing_fundamental":
        return norm(additive(hz, np.where(k == 1, 0.0, 1.0 / k), rng=rng)), "pitched"
    if kind == "vibrato":
        t = np.arange(n) / SR
        dev = 0.30 * np.sin(2 * np.pi * 5.5 * t)          # +-30 cents
        phase = 2 * np.pi * np.cumsum(hz * 2.0 ** (dev / 12.0)) / SR
        y = sum(np.sin(i * phase) / i for i in range(1, 21) if hz * i < SR / 2)
        return norm(y), "pitched"
    if kind == "pluck":                                     # Karplus-Strong 1983
        period = int(round(SR / hz))
        buf = rng.uniform(-1, 1, period)
        y = np.empty(n)
        for i in range(n):
            y[i] = buf[i % period]
            buf[i % period] = 0.498 * (buf[i % period] + buf[(i + 1) % period])
        return norm(y), "pitched"
    if kind.startswith("saw_noise"):
        snr = float(kind.split("_")[-1])
        s = additive(hz, 1.0 / k, rng=rng)
        w = pink(n, rng)
        w *= np.sqrt(np.mean(s ** 2) / np.mean(w ** 2)) * 10 ** (-snr / 20)
        return norm(s + w), "pitched"
    if kind == "bell":                                      # inharmonic, published minor-third bell ratios
        ratios = [0.5, 1.0, 1.2, 1.5, 2.0, 2.5, 2.67, 3.0, 4.0]
        amps = [0.7, 1.0, 0.8, 0.5, 0.6, 0.35, 0.3, 0.25, 0.2]
        decays = [0.5, 0.8, 1.0, 1.3, 1.6, 2.0, 2.2, 2.5, 3.0]
        return norm(additive(hz, amps, partial_ratios=ratios, decays=decays, rng=rng)), "ambiguous"
    if kind == "triad":                                     # root, major third, fifth
        y = sum(additive(hz * 2 ** (s / 12), 1.0 / k[:20], rng=rng) for s in (0, 4, 7))
        return norm(y), "ambiguous"
    if kind == "white":
        return norm(rng.standard_normal(n)), "unpitched"
    if kind == "pink":
        return norm(pink(n, rng)), "unpitched"
    if kind == "noise_burst":
        t = np.arange(n) / SR
        return norm(pink(n, rng) * np.exp(-t * 6)), "unpitched"
    raise ValueError(kind)


KINDS = ["sine", "saw", "square", "missing_fundamental", "vibrato", "pluck",
         "saw_noise_20", "saw_noise_10", "saw_noise_0", "bell", "triad",
         "white", "pink", "noise_burst"]


def classify(true_midi, s):
    if not P.is_stable(s):
        return "no_stable_f0"
    d = (s["dominant_midi"] - true_midi) * 100.0
    if abs(d) <= 50:
        return "correct"
    for mult in (-3, -2, -1, 1, 2, 3):
        if abs(d - 1200 * mult) <= 50:
            return "octave_error"
    return "other_error"


def true_midi(kind: str, midi: float) -> float:
    """The pitch a signal really has. Karplus-Strong's averaging filter adds half a sample
    to a loop of a whole number of samples: its pitch is SR / (P - 0.5), not the request."""
    if kind == "pluck":
        return float(P.hz_to_midi(SR / (round(SR / float(P.midi_to_hz(midi))) - 0.5)))
    return midi


def job(args):
    kind, asked, rep = args
    rng = np.random.default_rng(zlib.crc32(repr((kind, asked, rep)).encode()))
    y, group = make(kind, asked, rng)
    midi = true_midi(kind, asked)
    py = P.summarise_track(P.pyin_track(y, SR))
    cr = P.summarise_track(P.crepe_track(y, SR, device=CREPE_DEVICE))
    yn = P.shipped_yin(y, SR)
    row = {"kind": kind, "group": group, "midi": asked, "true_midi": midi, "rep": rep}
    for name, s in (("pyin", py), ("crepe", cr), ("yin", yn)):
        row[name] = s
        row[f"{name}_class"] = classify(midi, s)
        row[f"err_{name}"] = None if s["dominant_midi"] is None else (s["dominant_midi"] - midi) * 100
    return row


def offgrid(n=40):
    """pYIN's output sits on a grid of 10 cents anchored at fmin, so the error it shows at
    pitches that happen to fall on the grid flatters it. Saw tones at pitches drawn
    uniformly between C2 and C6, off any grid."""
    rng = np.random.default_rng(11)
    k = np.arange(1, 81)
    errs = {"pyin": [], "crepe": [], "yin": []}
    for midi in rng.uniform(36.0, 84.0, n):
        y = norm(additive(float(P.midi_to_hz(midi)), 1.0 / k, rng=rng))
        got = {"pyin": P.summarise_track(P.pyin_track(y, SR)),
               "crepe": P.summarise_track(P.crepe_track(y, SR, device=CREPE_DEVICE)),
               "yin": P.shipped_yin(y, SR)}
        for e, s in got.items():
            if s["dominant_midi"] is not None:
                errs[e].append(abs(s["dominant_midi"] - midi) * 100.0)
    print(f"| Estimator | Saw tones, random pitch | Median abs error | 95th percentile | Largest |")
    print("|---|---:|---:|---:|---:|")
    for e, v in errs.items():
        print(f"| {e} | {len(v)} of {n} | {np.median(v):.1f} c | {np.percentile(v, 95):.1f} c | {np.max(v):.1f} c |")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "offgrid":
        return offgrid()
    jobs = [(k, m, r) for k in KINDS for m in PITCHES for r in range(2)]
    rows = [job(j) for j in jobs]
    STUDY_DIR.mkdir(parents=True, exist_ok=True)
    (STUDY_DIR / "calibration.json").write_text(json.dumps(rows, indent=0))
    classes = ["correct", "octave_error", "other_error", "no_stable_f0"]
    print(f"{'signal':22} n   " + "   ".join(f"{e}:" + "/".join(c[:4] for c in classes) for e in ("pyin", "crepe", "yin")))
    for kind in KINDS:
        rs = [r for r in rows if r["kind"] == kind]
        line = f"{kind:22} {len(rs):3} "
        for e in ("pyin", "crepe", "yin"):
            cnt = [sum(r[f"{e}_class"] == c for r in rs) for c in classes]
            ok = [abs(r[f"err_{e}"]) for r in rs if r[f"{e}_class"] == "correct"]
            med = f"{np.median(ok):5.1f}c" if ok else "   - "
            line += f"  {e} " + "/".join(f"{c:2}" for c in cnt) + f" med|err|={med}"
        print(line)
    print("by pitch, pitched kinds only (correct of n):")
    for m in sorted(PITCHES):
        rs = [r for r in rows if r["midi"] == m and r["group"] == "pitched"]
        print(f"  midi {m:5.1f} {P.note_name(m):4} n={len(rs)} pyin={sum(r['pyin_class'] == 'correct' for r in rs)} "
              f"crepe={sum(r['crepe_class'] == 'correct' for r in rs)} "
              f"yin={sum(r['yin_class'] == 'correct' for r in rs)}")


if __name__ == "__main__":
    sys.exit(main())
