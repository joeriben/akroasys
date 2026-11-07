#!/usr/bin/env python3
"""Does an estimator follow a known shift on real generated audio?

    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_equivariance.py [n]
    ~/.cache/t5ynth-pitch-venv/bin/python tools/pitch_c3_equivariance.py channel

Generated audio has no ground truth, but a resampled copy of it does: moved by exactly
+150 cents, the estimate has to move by exactly that. An estimator that hops between
octaves or partials on this material shows up here, on the material itself rather than on
synthetic tones. Clips are drawn at random, seeded, from the matrix.
"""

from __future__ import annotations

import json
import random
import sys

import librosa
import numpy as np
import soundfile as sf

import pitch_c3_f0 as P
from pitch_c3_ipc import STUDY_DIR
from pitch_c3_tables import load, pct

SHIFT = 150.0


def channel_check(n=400):
    """The shipped code reads channel 0; this study measures the mono mix."""
    rows = [r for r in load("matrix") if "yin" in r and r["yin"]["dominant_midi"] is not None]
    random.Random(9).shuffle(rows)
    same = other = lost = stereo = 0
    for r in rows[:n]:
        y, sr = sf.read(STUDY_DIR / "matrix" / r["path"], dtype="float32", always_2d=True)
        if y.shape[1] < 2:
            continue
        stereo += 1
        s = P.shipped_yin(y[:, 0], sr)
        if s["dominant_midi"] is None:
            lost += 1
        elif abs(s["dominant_midi"] - r["yin"]["dominant_midi"]) <= 0.5:
            same += 1
        else:
            other += 1
    print(f"Shipped YIN on channel 0 against the mono mix, {stereo} stereo clips with a pitch from the mix: "
          f"same note {same}, other pitch {other}, no pitch {lost}.")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "channel":
        return channel_check()
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rows = [r for r in load("matrix") if r["group"] == "pitched"]
    random.Random(7).shuffle(rows)
    rows = rows[:n]
    root = STUDY_DIR / "matrix"
    ratio = 2.0 ** (SHIFT / 1200.0)
    out = {e: {"both": 0, "ok": 0, "octave": 0, "other": 0, "lost": 0, "was": 0, "err": []}
           for e in ("pyin", "crepe", "yin")}
    for r in rows:
        y, sr = sf.read(root / r["path"], dtype="float32", always_2d=True)
        y = P.to_mono(y)
        z = librosa.resample(y, orig_sr=sr, target_sr=sr / ratio, res_type="soxr_hq")
        moved = {"pyin": P.summarise_track(P.pyin_track(z, sr)),
                 "crepe": P.summarise_track(P.crepe_track(z, sr, device="mps")),
                 "yin": P.shipped_yin(z, sr)}
        for e, s in moved.items():
            o = out[e]
            if not P.is_stable(r[e]):
                continue
            o["was"] += 1
            if not P.is_stable(s):
                o["lost"] += 1
                continue
            o["both"] += 1
            d = (s["dominant_midi"] - r[e]["dominant_midi"]) * 100.0 - SHIFT
            if abs(d) <= 25.0:
                o["ok"] += 1
                o["err"].append(abs(d))
            elif any(abs(d - 1200.0 * m) <= 50.0 for m in (-3, -2, -1, 1, 2, 3)):
                o["octave"] += 1
            else:
                o["other"] += 1
    print(f"{len(rows)} clips, each moved by exactly +{SHIFT:.0f} cents.\n")
    print("| Estimator | Stable before | Lost after | Followed within 25 c | Median error | Octave hop | Other |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for e, o in out.items():
        med = f"{np.median(o['err']):.1f} c" if o["err"] else "–"
        print(f"| {e} | {o['was']} | {o['lost']} | {pct(o['ok'], o['both'])} | {med} | {pct(o['octave'], o['both'])} | {pct(o['other'], o['both'])} |")
    (STUDY_DIR / "equivariance.json").write_text(json.dumps({k: {a: b for a, b in v.items() if a != "err"} for k, v in out.items()}))


if __name__ == "__main__":
    main()
